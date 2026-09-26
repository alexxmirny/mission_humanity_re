#!/usr/bin/env python3
"""Determinism arms: the 2-peer UI-path lockstep gate and its shapes, the single-player oracle,
and the 3-peer / VM-only proofs.

    python tools/det_arms.py --determinism --ship-pacing
    python tools/det_arms.py --determinism --det-standard
    python tools/det_arms.py --sp-determinism --steps 800 --sp-seed 424242
    python tools/det_arms.py --u19j-gpfg3 --u19j-gpfg3-unguarded
    python tools/det_arms.py --det-selftest            # offline, a lint_repo row

Split out of tools/test_ui.py (tooling:TL-SUITE-SPLIT); `test_ui.py --determinism` etc. still
forward here.
"""

import check_orders_agree  # noqa: E402  TL-SUITE-FOLD-DETC1: the mh_orders.bin verdict
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lane_alloc  # noqa: E402  fork F4H: the ONE place a lane NUMBER comes from
import machine_config as machine  # noqa: E402
import make_lane  # noqa: E402  LANE_ROOT + the lane builder used by --local
import map_variant  # noqa: E402  mp:X2a -- the one-bit flip pushed to the client VM's own Maps\
import mp_run  # noqa: E402  mp:X2a -- scp the variant to/from the two independent rig VMs
from ui_suite_common import (  # noqa: E402
    BLIT_LOCAL_FPS_FLOOR,
    LOCAL_PORT_BASE,
    LOCAL_TIMEOUT_FRAMES,
    MODE_BROKERED,
    MODE_ORIGINAL,
    REPO,
    RunnerConfig,
    add_extra_ini_arg,
    add_harness_extra_arg,
    add_net_args,
    add_runner_args,
    add_steps_arg,
    apply_net_args,
    build_scenario_argv,
    frames_for_seconds,
    ini_file_mode,
    ini_selected_mode,
    parse_mode_args,
    print_desktop_banner,
    provision_lanes,
    run_ui_test,
    soak_ai_premise,
    sp_newest_run,
    vm_reachable,
    write_all_original_ini,
)

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


def peer_harness_config(det_dir, peer):
    """mp:D29 -- (configuration, manifest fp) from one peer's mh_harness.log: ("1"|"2", "XXXXXXXX"),
    or (None, None) when the harness never wrote its configuration line (not armed, refused, or a
    pre-D29 build). Read with mp_analyze's own regex so the two readers cannot drift."""
    path = os.path.join(det_dir, peer, "mh_harness.log")
    if not os.path.isfile(path):
        return None, None
    import mp_analyze as _ma

    # The needle, spelled here too because lint_log_formats reads it off each registered parser --
    # and asserted equal to mp_analyze's, so the two readers still cannot drift.
    rx = re.compile(r"^; \[harness\] configuration \(([12])\):.*\bmanifest fp=([0-9A-Fa-f]{8})\b")
    assert rx.pattern == _ma.HARNESS_CONFIG_RE.pattern, "D29 config-line regex drifted"
    cfg = fp = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for ln in f:
            m = rx.match(ln.rstrip())
            if m:
                cfg, fp = m.group(1), m.group(2).upper()
    return cfg, fp


def det_run_report(det_dir, want, configs=None):
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

    `configs` (mp:D29, optional) maps peer dir -> the harness CONFIGURATION that peer must report:
    "1" (no libmh.dll -- the spine-free harness) or "2". A configuration (1) shape passes
    want={peer: False} (nothing promoted -- there is nothing to promote) AND configs={peer: "1"},
    because the first alone would also pass a mode=original lane that still had libmh.dll beside it
    -- a configuration (1) verdict read off a configuration (2) run. Absent (no line) fails too: a
    peer whose harness never said which registry it hashed is not evidence for either.
    """
    lines, ok = [], True
    for peer, expect_cfg in (configs or {}).items():
        cfg, fp = peer_harness_config(det_dir, peer)
        lines.append(
            "      %-8s harness configuration: %s  manifest fp=%s"
            % (peer, "(%s)" % cfg if cfg else "(no line)", fp or "-")
        )
        if cfg != expect_cfg:
            ok = False
            lines.append(
                "      FAIL: %s was supposed to hash in configuration (%s) and reports %s -- this "
                "shape's verdict would describe the wrong build."
                % (peer, expect_cfg, "configuration (%s)" % cfg if cfg else "no configuration line")
            )
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
        compared = [p.get("overlap", 0) for p in (j.get("desync_pairs") or [])]
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


def run_sp_determinism(args, cfg):
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
        # clause (3): a deliberately wrong body on the second arm must drive this RED
        perturb = args.sp_perturb and name in ("promoted", "run_b")
        argv = build_scenario_argv(
            cfg=cfg,
            script=SP_SCRIPT,  # the single-peer form: `ui_test.py <script>`
            harness=True,
            steps=args.steps,
            harness_extra=hextra + ";" + args.sp_perturb if perturb else hextra,
            host_dir=lane_dir,
            # headless runs at thousands of fps: the 1500-frame default fires in seconds
            timeout_frames=LOCAL_TIMEOUT_FRAMES,
            timeout=max(args.timeout, 180 + args.steps),
            headless=not args.visible,
            extra_ini=[SP_PROMOTE_INI if promoted else SP_ROLLBACK_INI],
            net_extra=net_extra,
        )
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

        # mp:D29 -- THE CONFIGURATION (1) SHAPE. Nothing is promoted on either peer, and each
        # harness must SAY configuration (1): a mode=original lane that still carries libmh.dll
        # passes want={..: False} just as well, and its verdict would be a configuration (2) one.
        def hpeer(name, cfg):
            os.makedirs(os.path.join(d, name), exist_ok=True)
            with open(os.path.join(d, name, "mh_net.log"), "w", encoding="utf-8") as f:
                f.write("; nothing promoted here\n")
            with open(os.path.join(d, name, "mh_harness.log"), "w", encoding="utf-8") as f:
                f.write("; ==== mh replay harness armed: seed_step=1 ====\n")
                if cfg:
                    f.write(
                        "; [harness] configuration (%s): spine %s, registry=%s, rebased=0 owned=0, "
                        "uncovered=none, manifest fp=5A11D00D\n"
                        % (
                            cfg,
                            "ABSENT" if cfg == "1" else "PRESENT",
                            "mh.dll" if cfg == "1" else "libmh.dll",
                        )
                    )

        c1 = {"host": False, "client1": False}
        hpeer("host", "1")
        hpeer("client1", "1")
        check(
            "D29: a SYMMETRIC configuration (1) shape passes when both harnesses say (1)",
            det_run_report(d, c1, configs={"host": "1", "client1": "1"})[0],
        )
        hpeer("client1", "2")
        check(
            "D29: ...and FAILS when one peer hashed in configuration (2) (libmh.dll was present)",
            not det_run_report(d, c1, configs={"host": "1", "client1": "1"})[0],
        )
        check(
            "D29: a MIXED shape (host (1), client (2)) passes when asked for exactly that",
            det_run_report(d, c1, configs={"host": "1", "client1": "2"})[0],
        )
        hpeer("client1", None)
        check(
            "D29: a peer whose harness wrote NO configuration line fails the shape",
            not det_run_report(d, c1, configs={"host": "1", "client1": "1"})[0],
        )

        # mp:D29 O3 -- THE GO-RED ARM'S VERDICT. Each negative is a way the poked run could "fail"
        # (rc != 0) without proving the oracle saw the poke: no poke, NO DATA, red too early (the
        # clean arm was already red), red too late, or a widened region set (the poke changed
        # behaviour -- what the first O3 run's buildings-byte-0 poke did).
        def gored(poke_line, pairs):
            with open(os.path.join(d, "host", "mh_harness.log"), "a", encoding="utf-8") as f:
                if poke_line:
                    f.write(poke_line + "\n")
            with open(os.path.join(d, "host", "mp_analyze.json"), "w", encoding="utf-8") as f:
                json.dump({"desync_pairs": pairs}, f)
            return det_gored_verdict(d, 1500)[0]

        def pair(n, step, only):
            return {
                "pair": ["host", "client1"],
                "mismatch_count": n,
                "first_mismatch": {"step": step, "state_only_regions": only} if n else None,
            }

        poke = (
            "; REGION POKE step=1500 idx=55 players        @00E587E9+16 -> CD (D11 negative test)"
        )
        hpeer("host", "1")
        check(
            "D29 go-red: red at the poke step, players alone -> PASS",
            gored(poke, [pair(1501, 1500, ["players"])]),
        )
        hpeer("host", "1")
        check(
            "D29 go-red: the host never logged the poke -> FAIL",
            not gored(None, [pair(1501, 1500, ["players"])]),
        )
        hpeer("host", "1")
        check("D29 go-red: no compared pair (NO DATA) -> FAIL", not gored(poke, []))
        hpeer("host", "1")
        check(
            "D29 go-red: poked run IDENTICAL -> FAIL",
            not gored(poke, [pair(0, None, None)]),
        )
        hpeer("host", "1")
        check(
            "D29 go-red: first mismatch BEFORE the poke (already red) -> FAIL",
            not gored(poke, [pair(1800, 1200, ["players"])]),
        )
        hpeer("host", "1")
        check(
            "D29 go-red: first mismatch AFTER the poke -> FAIL",
            not gored(poke, [pair(10, 1600, ["players"])]),
        )
        hpeer("host", "1")
        check(
            "D29 go-red: the poke widened the diverging set (behaviour changed) -> FAIL",
            not gored(poke, [pair(1501, 1500, ["players", "buildings"])]),
        )
        hpeer("host", "1")
        check(
            "D29 go-red: the poke landed in a different region -> FAIL",
            not gored(poke.replace("players", "units"), [pair(1501, 1500, ["players"])]),
        )
        check(
            "D29 go-red: the poke region's index is players' in mp_analyze.REGION_NAMES",
            config1_poke_extra(1500)
            == "region_poke_at=1500;region_poke_only=%d;region_poke_off=16"
            % __import__("mp_analyze").REGION_NAMES.index("players"),
        )
        check(
            "D29: MIXED omits libmh on the HOST only; SYMMETRIC on both",
            [s[2] for s in CONFIG1_SHAPES] == [["libmh.dll"], ["host:libmh.dll"]]
            and CONFIG1_SHAPES[1][3] == UNPROMOTE_INI,
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

    # TL-HARN-CLEANCLOSE: the runner's straggler wait. Silent when it works (every peer closed) and
    # silent when it is wrong in the other direction too (a pull taken before a client's stop reads as
    # "no rollups" -- the exact misreading D31/U41c made), so both directions are pinned here, with
    # the pull stubbed: `later` is what a peer's folder holds once it has reached its stop step.
    import ui_test as _uc

    d = tempfile.mkdtemp(prefix="det_close_")
    real_pull = _uc.pull_peer_logs
    try:
        stop_ln = "[1] ; [session] HARNESS_STOP step=1500 close=in_place -- x\n"
        later = {}

        def put(key, harness, net):
            os.makedirs(os.path.join(d, key), exist_ok=True)
            with open(os.path.join(d, key, "mh_harness.log"), "w") as f:
                f.write(harness)
            with open(os.path.join(d, key, "mh_net.log"), "w") as f:
                f.write(net)

        pulls = []

        def fake_pull(args, ip, run, dest):
            key = os.path.basename(dest)
            pulls.append(key)
            put(key, *later[key])
            return dest

        _uc.pull_peer_logs = fake_pull
        _uc.CLOSE_WAIT_S, saved_wait = 3, _uc.CLOSE_WAIT_S
        done_h = "1 x\n; per-region breakdown at stop step 1500:\n"
        peers = [("host", "h", "r0"), ("client1", "c", "r1"), ("client2", "q", "r2")]
        # host closed; client1 was pulled one step short and closes a moment later; client2 is the
        # excluded quitter and must never be waited on.
        put("host", done_h, stop_ln)
        put("client1", "1 x\n", "")
        put("client2", "1 x\n", "")
        later["client1"] = (done_h, stop_ln)
        got = _uc.det_await_close(None, peers, d, ["client2"])
        check(
            "CLOSE: a client pulled before its stop step is re-pulled and its close found",
            got.get("client1") == (1500, "in_place") and pulls == ["client1"],
        )
        check("CLOSE: the host that already closed is not re-pulled", "host" not in pulls)
        check("CLOSE: an excluded peer (the quitter) is never waited on", "client2" not in got)
        # a peer that reached its stop step but wrote no close line (close_on_stop=0, an older mh.dll)
        # is reported ABSENT at once -- not waited on for the whole budget.
        del pulls[:]
        put("client1", done_h, "")
        got = _uc.det_await_close(None, peers[:2], d, [])
        check(
            "CLOSE: a finished peer with no close line reads ABSENT without a re-pull",
            got.get("client1") is None and pulls == [],
        )
        check(
            "CLOSE: the line's step and close= are what the runner reports",
            _uc.harness_stop_of(os.path.join(d, "host")) == (1500, "in_place"),
        )
    finally:
        _uc.pull_peer_logs = real_pull
        _uc.CLOSE_WAIT_S = saved_wait
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


def run_det_3peer(args, cfg):
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
    if not cfg.stock_exe:
        cmd.append("--patched-exe")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return False, [
            "      FAIL: lane %s: %s" % (DET3_LANE, (r.stderr or r.stdout).strip()[:200])
        ]
    argv = build_scenario_argv(
        cfg=cfg,
        determinism=True,
        steps=args.steps,
        host="%s:%s" % (args.vms[0], DET3_HOST_SCRIPT),
        clients=[
            "%s:%s" % (args.vms[1], DET3_CLIENT_SCRIPTS[0]),
            "lane=%s:%s" % (DET3_LANE, DET3_CLIENT_SCRIPTS[1]),
        ],
        connect_ip=args.vms[0],
        timeout_frames=LOCAL_TIMEOUT_FRAMES,
        # peers=2 is this shape's own requirement; the caller's --net-extra is APPENDED (D24)
        net_extra=";".join(["peers=2"] + ([args.net_extra] if args.net_extra else [])),
        timeout=max(args.timeout, 120 + args.steps),
        extra_ini=[PROMOTE_INI],
        # MP D24: opt-in order recording (mh_orders.bin is pulled back for every peer)
        record=getattr(args, "record", 0),
    )
    det_clear(det_dir)
    rc = run_ui_test(argv, max(args.per_test_timeout, 180 + args.steps))[0]
    ok, lines = det_run_report(det_dir, {"host": True, "client1": True, "client2": True})
    bok, blines = det3_barrier_report(det_dir)
    return (rc == 0 and ok and bok), lines + blines


# mp:U19b -- 3-peer clean quit. `graceful_quit` (U19) is 2-peer, and an AI seat does not count
# toward the quorum llm_net_player_remove tests (mp_host_gquit.txt's header: measured with occ 3 and
# an AI visibly seated, the removal still ended the match). U19's own third clause -- survivors that
# keep PLAYING past a departure -- needs a real third peer, and local 3-peer discovery does not seat
# a second 127.0.0.1 client (measured during mp:GS2's own 3-peer clause, 2026-09-21). So this reuses
# the SAME VM+VM+local-lane topology as the U28 3-peer barrier above (host vms[0], one survivor
# client vms[1], the QUITTER as a local lane) -- literally the SAME lane (DET3_LANE/DET3_LANE_NO/
# DET3_PORT), per the wave-2 brief's "share_lanes of an existing 3-peer row" (dead-ends G259: the
# lane pool has no headroom for a new one). The two shapes therefore cannot run concurrently, which
# is fine: both are manually-invoked CLI shapes, never part of the parallel default suite.
def run_u19b_quit3(args, cfg):
    """The U19b 3-peer clean-quit shape. Returns (ok, lines), same contract as run_det_3peer."""
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
    if not cfg.stock_exe:
        cmd.append("--patched-exe")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return False, [
            "      FAIL: lane %s: %s" % (DET3_LANE, (r.stderr or r.stdout).strip()[:200])
        ]
    argv = build_scenario_argv(
        cfg=cfg,
        determinism=True,
        steps=args.steps,
        host="%s:mp_host_quit3.txt" % args.vms[0],
        clients=[
            "%s:mp_client_quit3_survivor.txt" % args.vms[1],
            "lane=%s:mp_client_quit3_quitter.txt" % DET3_LANE,
        ],
        # the quitter's log is short by design: keep it, compare only the two survivors
        det_exclude=["client2"],
        connect_ip=args.vms[0],
        timeout_frames=LOCAL_TIMEOUT_FRAMES,
        # peers=2 seats the third slot; graceful_leave explicit so the mechanism stays exercised
        net_extra=";".join(
            ["peers=2", "transport=tcp", "graceful_leave=1"]
            + ([args.net_extra] if args.net_extra else [])
        ),
        timeout=max(args.timeout, 120 + args.steps),
        extra_ini=[PROMOTE_INI],
        # hash rows for mp_analyze; no random workload in the walk
        harness_extra="region_hash_step=50;synth_move=0",
    )
    det_clear(det_dir)
    rc = run_ui_test(argv, max(args.per_test_timeout, 180 + args.steps))[0]
    ok, lines = det_run_report(det_dir, {"host": True, "client1": True, "client2": True})
    import check_quit_survivors_3peer as _q3

    host_dir = os.path.join(det_dir, "host")
    client1_dir = os.path.join(det_dir, "client1")
    client2_dir = os.path.join(det_dir, "client2")
    try:
        qok, qlines = _q3.check(host_dir, client1_dir, client2_dir)
    except _q3.Refusal as exc:
        qok, qlines = False, ["      REFUSED: %s" % exc]
    lines = lines + ["   check_quit_survivors_3peer:"] + ["      " + ln for ln in qlines]
    return (rc == 0 and ok and qok), lines


# mp:L1f -- THE 3-PEER LOBBY-PING SHAPE: every player sees every OTHER player's ping.
#
# WHY IT IS A CLI SHAPE AND NOT A REGISTRY ROW, stated because COMMON3 rule 7 says rows share lanes:
#   * THREE PEERS ON THIS RIG MEANS host on vms[0], one client on vms[1], the third as a LOCAL LANE.
#     Local 3-peer DISCOVERY does not seat a second 127.0.0.1 client (measured at mp:GS2's own
#     3-peer clause, 2026-09-21), and the rig has two VMs, not three -- so the brokered topology the
#     suite's `multi` rows use cannot carry this walk at all. The DET3 lane is the established
#     answer (U28's barrier shape, then mp:U19b's clean quit), and this shape reuses it verbatim:
#     same lane, same port, same "cannot run concurrently with the other two" caveat.
#   * It is therefore the SAME class as those two -- a manually-invoked shape whose verdict is a
#     checker's, not a pixel diff's -- and it costs the 81/81 suite lane block nothing.
# SHIP CONFIGURATION (COMMON3 rule 6): transport=udp, defang_overlay=0 (redundant since
# TL-RIG-DEFANG made it ui_test's default; spelled anyway, this shape is invoked by hand), no
# harness knobs. The walk
# never leaves the LOBBY, so there is no sim to be deterministic about and no libmh to arm: the
# whole mechanism (the host's publication and the cell that renders it) is lobby-only by
# construction (net_seams.cpp drives both from on_lobby_dispatch).
L1F_HOST_SCRIPT = "mp_host_ping3.txt"
L1F_CLIENT_SCRIPTS = ("mp_client_ping3.txt", "mp_client_ping3b.txt")


def l1f_log_dir():
    """Where this shape's three peers' logs are pulled to -- one directory per ui_test peer KEY
    (`host`, `client1`, `client2`), the same naming the determinism path uses. Its own directory
    and not the shared determinism one: that one is wiped by det_clear, and this is not a
    determinism shape."""
    return os.path.join(REPO, "tmp", "ui_test", "l1f_ping3")


def run_l1f_ping3(args, cfg):
    """mp:L1f's 3-peer lobby shape. Returns (ok, lines), same contract as run_u19b_quit3."""
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
    if not cfg.stock_exe:
        cmd.append("--patched-exe")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return False, [
            "      FAIL: lane %s: %s" % (DET3_LANE, (r.stderr or r.stdout).strip()[:200])
        ]
    # NOT RELAYED. This shape is the DIRECT arm, and `--relay` is deliberately not honoured here:
    # a relayed 3-peer lobby needs the relay's directory join walk rather than the IP-join these
    # scripts use, and an arm that half-works would prove less than saying so. The relayed answer
    # (`relayed = 1`) is proven OFFLINE instead, where a tunnel can be driven exactly --
    # `net_selftest.exe udprelaytest`, the path_class arm. See tracker mp:L1f.
    argv = build_scenario_argv(
        cfg=cfg,
        host="%s:%s" % (args.vms[0], L1F_HOST_SCRIPT),
        clients=[
            "%s:%s" % (args.vms[1], L1F_CLIENT_SCRIPTS[0]),
            "lane=%s:%s" % (DET3_LANE, L1F_CLIENT_SCRIPTS[1]),
        ],
        connect_ip=args.vms[0],
        port=DET3_PORT,
        # a headless lane outruns the 1500-frame default before the menu exists
        timeout_frames=LOCAL_TIMEOUT_FRAMES,
        # peers=2 seats the third slot; the rest is the SHIP shape
        net_extra=";".join(
            ["peers=2", "transport=udp", "defang_overlay=0"]
            + ([args.net_extra] if args.net_extra else [])
        ),
        extra_ini=["tools/uiscripts/ini/video_1024.ini"],
        # two of the peers are VMs: their logs are the evidence
        pull_logs=l1f_log_dir(),
    )
    shutil.rmtree(l1f_log_dir(), ignore_errors=True)
    rc = run_ui_test(argv, max(args.per_test_timeout, 420))[0]
    # ui_test's PEER KEYS, not the determinism path's: a client's key is its IP (a VM) or its lane
    # name (a local lane), and only the host is called "host". Naming them here rather than globbing
    # keeps a peer that produced nothing an obvious absence instead of a two-peer pass.
    dirs = [os.path.join(l1f_log_dir(), k) for k in ("host", args.vms[1], DET3_LANE)]
    lines = ["   peer logs:"] + ["      %s" % d for d in dirs]
    missing = [d for d in dirs if not os.path.isfile(os.path.join(d, "mh_net.log"))]
    if missing:
        return False, lines + [
            "      FAIL: no mh_net.log pulled for %s"
            % ", ".join(os.path.basename(d) for d in missing)
        ]
    chk = subprocess.run(
        [
            sys.executable,
            os.path.join(REPO, "tools", "check_lobby_ping.py"),
            *dirs,
            "--published",
            "--every-slot",
            # 250 ms: a LAN rig's SRTT is single-digit ms, so this is wide enough that a scheduling
            # hiccup on one peer cannot fail the row and narrow enough that a wrong slot, a stale
            # table or a units slip still does. The window is the claim's tolerance, not its subject.
            "--agree",
            "250",
        ],
        capture_output=True,
        text=True,
        cwd=REPO,
    )
    lines += ["   check_lobby_ping:"] + [
        "      " + ln for ln in (chk.stdout or chk.stderr).strip().splitlines()
    ]
    return (rc == 0 and chk.returncode == 0), lines


# mp:U19j -- THE GONE-PEER FRAME GUARD'S RIG PROOF: a configuration-(1) run in which the byte-patch
# carrier (mp:U19i, the splice over llm_net_lockstep_dispatch's kick re-broadcast @0x0049c330) FIRES.
#
# WHY IT IS A 3-PEER CLI SHAPE AND NOT A REGISTRY ROW. The call runs only when a frame arrives from a
# sender the leader has ALREADY written off while the leader is still in lockstep (session mode 3).
# A 2-peer removal ends the survivor's match on the removal frame itself, so no later frame is ever
# dispatched (dead-ends G300: 184 survivor logs PATCHED, 0 FIRED). It takes a third peer to keep the
# match alive past a drop, and three peers on this rig means the DET3 topology (host vms[0], client
# vms[1], the third as the local det3_client2 lane) -- local 3-peer discovery cannot seat a second
# 127.0.0.1 client, so no `multi` row can carry it (--u19b-quit3 / --l1f-ping3's reason, same lane).
#
# THE SHAPE, measured once before it was a test (mp:GS2's 3-peer clause, 2026-09-21, promoted: the
# host logged `[promote] wire/send_lockstep_kick: call #1` 2 ms after its GS2 drop, with the fenced
# peer's heartbeats still arriving at ~20/s): the LOCAL lane fences its sim (`simstep`) with its
# transport up; the host and vms[1] drop it after data_timeout_ms=3000 of horizon silence and play on
# (count_active_players == 2, so session 3 persists); the fenced peer's horizon-heartbeat thread keeps
# sending 9-byte type-0x02 adverts, and each one reaching the host (the leader: slot 0) goes through
# the gone-peer branch. Guarded, the carrier logs FIRED and the host keeps playing; unguarded
# (`gone_peer_frame_guard=0`), the kick's six bytes overwrite the advert's head, offset 6 is read as
# an outer tag, and the host's own dispatch raises outcome 7 inside session 3 -- the garbled frame.
#
# CONFIGURATION (1) ON ALL THREE: --omit-satellite libmh.dll (deleted on the VMs, mp:D29) + a lane
# built without it; transport=udp; defang_overlay=0. The harness is armed only for `simstep` (the
# fence lives in mh_harness.dll, not in libmh). Verdict: tools/check_gone_peer_guard.py over the
# three peers' pulled logs; the unguarded arm is EXPECT-RED (XFAIL only when it went red on the
# garbled frame -- an unguarded arm that stays clean is a failure, G283/G285).
U19J_HOST_SCRIPT = "mp_host_gpfg3.txt"
U19J_SURVIVOR_SCRIPT = "mp_client_gpfg3_survivor.txt"
U19J_FENCED_SCRIPT = "mp_client_gpfg3_frozen.txt"


def u19j_log_dir(guarded):
    return os.path.join(REPO, "tmp", "ui_test", "u19j_gpfg3" + ("" if guarded else "_unguarded"))


def run_u19j_gpfg3(args, cfg, guarded=True):
    """mp:U19j's 3-peer carrier-FIRES shape. Returns (ok, lines); `ok` for the unguarded arm is the
    EXPECT-RED verdict (True = XFAIL on the garbled frame)."""
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
        # configuration (1) on the local peer: the lane is BUILT without libmh.dll (the other DET3
        # shapes re-provision it with every satellite, so this does not leak into them).
        "--omit-satellite",
        "libmh.dll",
    ]
    if not cfg.stock_exe:
        cmd.append("--patched-exe")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return False, [
            "      FAIL: lane %s: %s" % (DET3_LANE, (r.stderr or r.stdout).strip()[:200])
        ]
    net = ["peers=2", "transport=udp", "defang_overlay=0", "data_timeout_ms=3000"]
    if not guarded:
        net.append("gone_peer_frame_guard=0")
    if args.net_extra:
        net.append(args.net_extra)
    log_dir = u19j_log_dir(guarded)
    argv = build_scenario_argv(
        cfg=cfg,
        host="%s:%s" % (args.vms[0], U19J_HOST_SCRIPT),
        clients=[
            "%s:%s" % (args.vms[1], U19J_SURVIVOR_SCRIPT),
            "lane=%s:%s" % (DET3_LANE, U19J_FENCED_SCRIPT),
        ],
        connect_ip=args.vms[0],
        port=DET3_PORT,
        timeout_frames=LOCAL_TIMEOUT_FRAMES,
        # two VM deploys + a 3-peer lobby + the match; the 90 s default expired mid-match
        timeout=300,
        omit_satellite=["libmh.dll"],
        net_extra=";".join(net),
        # simstep needs an armed harness; no workload, no per-step hash lines
        harness_extra="synth_move=0;region_hash_step=0",
        # the lobby frame is mp_host_ping3's, whose masks are 1024-wide
        extra_ini=["tools/uiscripts/ini/video_1024.ini"],
        pull_logs=log_dir,
        update_baselines=args.update_baselines,
    )
    shutil.rmtree(log_dir, ignore_errors=True)
    rc = run_ui_test(argv, max(args.per_test_timeout, 420))[0]
    dirs = [os.path.join(log_dir, k) for k in ("host", args.vms[1], DET3_LANE)]
    lines = ["   ui_test rc=%d%s" % (rc, "" if guarded else " (a red here is the expected half)")]
    lines += ["   peer logs:"] + ["      %s" % d for d in dirs]
    chk_argv = [sys.executable, os.path.join(REPO, "tools", "check_gone_peer_guard.py"), *dirs]
    chk_argv += ["--arm", "on"] if guarded else ["--arm", "off", "--expect-red"]
    chk = subprocess.run(chk_argv, capture_output=True, text=True, cwd=REPO)
    lines += ["   check_gone_peer_guard:"] + [
        "      " + ln for ln in (chk.stdout or chk.stderr).strip().splitlines()
    ]
    if guarded:
        return (rc == 0 and chk.returncode == 0), lines
    # EXPECT-RED: the checker's own XFAIL is the verdict. ui_test's rc is not consulted -- the host's
    # `absent Continue game` is SUPPOSED to fail when the frame garbles.
    return chk.returncode == 0, lines


# mp:X2a -- THE OPEN-REDIRECT, WITNESSED FOR REAL, on the two independent rig VMs.
#
# WHY THE EXISTING X2 SCENARIOS (map_absent/map_conflict, TESTS registry) DO NOT CLOSE THIS. Every
# local lane's `Maps` is a SYMLINK to the one shared game image (make_lane.py), so those scenarios
# stage the client's starting state with a net-only knob (`[net] map_test_pretend=none|other`) that
# "moves no file" -- the client's OWN base file IS the host's real content the whole time. A broken
# open-redirect (map_transfer.cpp's `utils_open_file` replacement) would therefore still open
# matching bytes and those scenarios would still read green; see check_map_transfer.py's own
# docstring. The redirect's only proof until now was `net_selftest.exe maptest` arms E/W, offline.
#
# WHY IT NEEDS THE TWO REAL VMs AND NOT ANOTHER LOCAL LANE. `resolve()` (map_transfer.cpp) with the
# pretend knob OFF hashes the client's ACTUAL local file and compares it against the host's claim --
# the genuine production path. That only diverges from the host's content if the client's file
# genuinely differs, and a local lane cannot do that without either breaking the symlink (X2's own
# dead end: the first `pretend` version tried exactly this and took the map away from the HOST too)
# or copying a whole second game image. The two rig VMs (.37/.38) are independent installs already,
# so pushing tools/map_variant.py's one-bit flip into ONLY the client's `Maps\blue monday.mpm` makes
# the divergence real without touching anything shared.
#
# THE MUTATION IS BACKED UP AND RESTORED, because a VM has no per-lane folder (unlike a local lane,
# whose own copy make_lane.py's `--map-variant` mutates) -- its Maps\ is the ONE persistent install
# every other VM-based run also uses. The backup is PULLED (the peer's actual current bytes), not
# assumed equal to this box's copy, and the restore runs in a `finally` so a timeout or a checker
# crash never leaves the shared install mutated.
#
# NO net_extra KNOB IS NEEDED: with map_test_pretend unset (PRETEND_OFF), resolve()'s base-file
# compare is the real one, so mp_host_map.txt / mp_client_map.txt (X2's own scripts) are reused
# unchanged -- their walk does not care whether the mismatch is staged or genuine.
X2A_MAP_NAME = "blue monday.mpm"  # the map picker's pre-selected entry; both scripts build on it


def x2a_log_dir():
    return os.path.join(REPO, "tmp", "ui_test", "x2a_map_variant")


def run_x2a_map_variant(args, cfg):
    """mp:X2a's rig proof. Returns (ok, lines); `ok` is None for a VM-unreachable SKIP."""
    down = [ip for ip in args.vms[:2] if not vm_reachable(ip)]
    if down:
        return None, ["      SKIP -- VM(s) unreachable: %s" % ", ".join(down)]
    host_ip, client_ip = args.vms[0], args.vms[1]
    fwd = machine.VM_DIR.replace("\\", "/")
    remote_map = "%s/Maps/%s" % (fwd, X2A_MAP_NAME)
    scratch = os.path.join(REPO, "tmp", "x2a_map_variant")
    os.makedirs(scratch, exist_ok=True)
    backup_local = os.path.join(scratch, "backup.mpm")
    variant_local = os.path.join(scratch, "variant.mpm")

    lines = []
    pull = mp_run.scp(
        machine.SSH_KEY, "%s@%s:%s" % (machine.VM_USER, client_ip, remote_map), backup_local
    )
    if pull.returncode != 0 or not os.path.isfile(backup_local):
        return False, [
            "      FAIL: could not pull the client's own %s to back it up first -- refusing to "
            "mutate a persistent VM install this run could not restore" % X2A_MAP_NAME
        ]
    with open(backup_local, "rb") as f:
        orig = f.read()
    variant = map_variant.variant_bytes(orig)
    with open(variant_local, "wb") as f:
        f.write(variant)
    lines.append(
        "      %s backed up (%d B): base hash=%s variant hash=%s"
        % (
            X2A_MAP_NAME,
            len(orig),
            map_variant.content_hash8(orig),
            map_variant.content_hash8(variant),
        )
    )

    restored = [False]

    def restore():
        if restored[0]:
            return
        rr = mp_run.scp(
            machine.SSH_KEY, backup_local, "%s@%s:%s" % (machine.VM_USER, client_ip, remote_map)
        )
        restored[0] = rr.returncode == 0
        lines.append(
            "      restore %s: %s"
            % (
                X2A_MAP_NAME,
                "ok"
                if restored[0]
                else "FAILED -- the client VM's own map is left mutated, fix by hand (%s)"
                % client_ip,
            )
        )

    try:
        push = mp_run.scp(
            machine.SSH_KEY, variant_local, "%s@%s:%s" % (machine.VM_USER, client_ip, remote_map)
        )
        if push.returncode != 0:
            return False, lines + ["      FAIL: could not push the variant to the client VM"]

        # THE VM'S mh_dl\ IS PERSISTENT (unlike a local lane, which is fresh per test), so an
        # earlier run's download of this exact content (map_absent/map_conflict/a previous X2a run
        # -- the real host content is the same file every time) leaves `resolve()` a Resolve::Stored
        # candidate it can satisfy WITHOUT a transfer -- measured first run: host never armed a
        # transfer, Start never gated, and the redirect fired off a STALE cache instead of THIS
        # run's download (mp:T5/T6's same trap, one level up: a cache that makes the mechanism look
        # exercised when it was not). Clearing first is what makes "redirect armed" mean "THIS run's
        # download landed", not "something armed it once, on some earlier occasion".
        mp_run.ssh(  # a leftover process could hold a handle under mh_dl\ (TL-RIGKILL's reason)
            machine.SSH_KEY,
            machine.VM_USER,
            client_ip,
            "taskkill /im mh.focus.exe /f 2>nul & schtasks /delete /tn uitest /f 2>nul",
        )
        clear = mp_run.ssh(
            machine.SSH_KEY,
            machine.VM_USER,
            client_ip,
            'rmdir /s /q "%s\\mh_dl" 2>nul' % machine.VM_DIR,
        )
        lines.append(
            "      client mh_dl\\ cleared: %s"
            % ("ok" if clear.returncode == 0 else "already absent")
        )

        log_dir = x2a_log_dir()
        shutil.rmtree(log_dir, ignore_errors=True)
        argv = build_scenario_argv(
            cfg=cfg,
            host="%s:mp_host_map.txt" % host_ip,
            clients=["%s:mp_client_map.txt" % client_ip],
            connect_ip=host_ip,
            timeout_frames=LOCAL_TIMEOUT_FRAMES,
            timeout=300,
            net_extra="transport=udp",
            extra_ini=["tools/uiscripts/ini/video_1024.ini"],
            pull_logs=log_dir,
            update_baselines=args.update_baselines,
        )
        rc = run_ui_test(argv, max(args.per_test_timeout, 420))[0]
        dirs = [os.path.join(log_dir, k) for k in ("host", client_ip)]
        lines.append("   ui_test rc=%d" % rc)
        lines += ["   peer logs:"] + ["      %s" % d for d in dirs]
        chk = subprocess.run(
            [sys.executable, os.path.join(REPO, "tools", "check_map_redirect.py"), *dirs],
            capture_output=True,
            text=True,
            cwd=REPO,
        )
        lines += ["   check_map_redirect:"] + [
            "      " + ln for ln in (chk.stdout or chk.stderr).strip().splitlines()
        ]
        return (rc == 0 and chk.returncode == 0), lines
    finally:
        restore()


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


# ---- mp:D29 O3: the CONFIGURATION (1) determinism shapes ----------------------------------------
# Configuration (1) is the build players run: mh.dll WITHOUT libmh.dll. Until D29 the harness refused
# to arm there (ruling Q4), so every IDENTICAL verdict came from a libmh-present peer. Two shapes:
#   SYMMETRIC (1)  -- both peers without libmh.dll: players' build against itself.
#   MIXED          -- (1) on the host vs mode=original WITH libmh on the client: the only shape that
#                     tests the assumption "configuration (1) behaves like mode=original" directly.
# Each runs TWICE: clean (must be ALL PAIRS IDENTICAL, with both harnesses naming the configuration
# asked for) and a GO-RED arm (a host-only poke must turn the pair red at exactly the poke step, in
# exactly the poked region) -- so neither shape can pass by hashing nothing.
#
# THE POKE IS players+0x10 (name[32]), NOT A LIVE REGION. The first O3 go-red run poked
# `buildings` byte 0 (region_poke_only=0, the O2 lane's suggestion): that is live state, it changed
# behaviour, the client was eliminated at ~1504 and the match ended (on_gameover, outcome 8) -- the
# host's harness log then stopped at 1450 and mp_analyze had too few common steps to compare at all
# (NO DATA, "RUN VALID: NO"). name[32] is read by nothing, so the match keeps running and the pair
# stays red from the poke to the end (D23 proved the same recipe; mp_analyze's `players` note).
CONFIG1_POKE_REGION = "players"
CONFIG1_POKE_OFF = 16
CONFIG1_SHAPES = [
    # (label, why, omit_satellite specs (ui_test syntax), extra_ini_client, configs)
    (
        "SYMMETRIC CONFIGURATION (1)",
        "both peers WITHOUT libmh.dll -- the build players run, against itself",
        ["libmh.dll"],
        None,
        {"host": "1", "client1": "1"},
    ),
    (
        "MIXED (1) vs mode=original",
        "host WITHOUT libmh.dll vs client mode=original WITH it -- does players' build step like "
        "the original? A red here is the first measured difference: file it, never wave it through",
        ["host:libmh.dll"],
        UNPROMOTE_INI,
        {"host": "1", "client1": "2"},
    ),
]


def config1_poke_step(steps):
    """The go-red arm's poke step: mid-run, so both an identical prefix and a red tail are compared."""
    return max(100, min(1500, steps // 2))


def config1_poke_extra(poke_step):
    import mp_analyze as _ma

    return "region_poke_at=%d;region_poke_only=%d;region_poke_off=%d" % (
        poke_step,
        _ma.REGION_NAMES.index(CONFIG1_POKE_REGION),
        CONFIG1_POKE_OFF,
    )


def det_gored_verdict(det_dir, poke_step, region=CONFIG1_POKE_REGION):
    """mp:D29 -- did a host-only poke turn the pair red EXACTLY where it was placed? (ok, lines).

    All four must hold, because each failure mode is a way for the go-red arm to "pass" while proving
    nothing about the oracle:
      * the host logged `REGION POKE step=<poke_step>` for `region` (the poke really happened there);
      * mp_analyze compared the pair and found a state mismatch (not NO DATA -- a run that ended
        early reads as "not clean" too, and would pass a bare rc!=0 check);
      * the FIRST state mismatch is AT the poke step -- earlier means the pair was already red
        (the clean arm's verdict is then suspect), later means the poke was not seen;
      * the state-only diverging set at that step is exactly [region] -- a wider set means the poke
        changed behaviour, and localization is lost.
    """
    lines, ok = [], True
    hl = os.path.join(det_dir, "host", "mh_harness.log")
    rx = re.compile(r"^; REGION POKE step=(\d+) idx=\d+ (\S+)")
    pokes = []
    if os.path.isfile(hl):
        with open(hl, encoding="utf-8", errors="replace") as f:
            for ln in f:
                m = rx.match(ln)
                if m:
                    pokes.append((int(m.group(1)), m.group(2)))
    if (poke_step, region) not in pokes:
        ok = False
        lines.append(
            "      FAIL: the host logged no REGION POKE step=%d %s (saw %s)"
            % (poke_step, region, pokes or "none")
        )
    try:
        with open(os.path.join(det_dir, "host", "mp_analyze.json"), encoding="utf-8") as f:
            pairs = json.load(f).get("desync_pairs") or []
    except Exception:
        pairs = []
    if not pairs:
        lines.append("      FAIL: mp_analyze.json has no compared pair")
        return False, lines
    for p in pairs:
        fm = p.get("first_mismatch") or {}
        step, only = fm.get("step"), fm.get("state_only_regions")
        lines.append(
            "      go-red %s: state-mismatch steps=%s first=%s state-only regions=%s"
            % ("/".join(p.get("pair") or []), p.get("mismatch_count"), step, only)
        )
        if not p.get("mismatch_count"):
            ok = False
            lines.append(
                "      FAIL: the poked run is IDENTICAL -- the oracle did not see the poke"
            )
        elif step != poke_step:
            ok = False
            lines.append("      FAIL: first mismatch at %s, poke at %d" % (step, poke_step))
        elif only != [region]:
            ok = False
            lines.append(
                "      FAIL: diverging state set %s is not exactly [%s] -- localization lost"
                % (only, region)
            )
    return ok, lines


# tooling:TL-SUITE-FOLD-DETC1: the decoder + verdict live in check_orders_agree.py (one copy).
det_orders_verdict = check_orders_agree.det_orders_verdict


def run_det_config1(args, cfg, local=False, gate=False):
    """mp:D29 O3 -- both configuration (1) shapes, clean + go-red each. Returns [(label, ok, lines)],
    or None when the VM pair is unreachable (SKIP). `local` runs them on two lanes of this box
    (make_lane --omit-satellite, since a lane's satellites are a property of the folder)."""
    if not local:
        down = [ip for ip in args.vms[:2] if not vm_reachable(ip)]
        if down:
            print("[det] configuration (1) SKIP -- VM(s) unreachable: %s" % ", ".join(down))
            return None
    det_dir = os.path.join(REPO, "tmp", "ui_test", "determinism")
    poke = config1_poke_step(args.steps)
    results = []
    # `gate` (--det-config1-gate, run_gate's det_c1 unit): the SYMMETRIC shape's clean arm only --
    # the build players run, against itself, every gate. MIXED and both go-red arms stay in
    # --det-standard / --det-config1 (user, 2026-09-24: ~2.5 min per gate, not ~10).
    shapes = CONFIG1_SHAPES[:1] if gate else CONFIG1_SHAPES
    for label, why, omit, extra_client, configs in shapes:
        for gored in (False,) if gate else (False, True):
            tag = "%s%s%s" % (
                label,
                " [go-red @%d]" % poke if gored else "",
                " (local)" if local else "",
            )
            print("\n" + "=" * 78)
            print("[det] SHAPE: %s\n      %s" % (tag, why))
            print("=" * 78)
            if local:
                det_test = {
                    "name": "determinism",
                    "kind": "multi",
                    "host": "mp_host_start.txt",
                    "clients": ["mp_client_start.txt"],
                    "omit_satellite": omit,
                }
                plan = provision_lanes(
                    [det_test],
                    headless=False,
                    port_base=DET_LOCAL_PORT,
                    lane_base=DET_LOCAL_LANE_BASE,
                    stock_exe=cfg.stock_exe,
                )
                if not plan:
                    results.append((tag, False, ["      FAIL: lane provisioning failed"]))
                    continue
                port, _, names = plan["determinism"]
                where = dict(
                    host="lane=%s:mp_host_start.txt" % names[0],
                    clients=["lane=%s:mp_client_start.txt" % names[1]],
                    connect_ip="127.0.0.1",
                    port=port,
                )
            else:
                where = dict(
                    host="%s:mp_host_start.txt" % args.vms[0],
                    clients=["%s:mp_client_start.txt" % args.vms[1]],
                    connect_ip=args.vms[0],
                    timeout_frames=frames_for_seconds(240, BLIT_LOCAL_FPS_FLOOR),
                    omit_satellite=list(omit),
                )
            argv = build_scenario_argv(
                cfg=cfg,
                determinism=True,
                steps=args.steps,
                timeout=max(args.timeout, 120 + args.steps),
                extra_ini_client=extra_client,
                ship_pacing=args.ship_pacing,
                harness_extra_host=config1_poke_extra(poke) if gored else None,
                # dist:V022: the clean arm records orders on both peers (order_mode=1)
                record=None if gored else 1,
                **where,
            )
            det_clear(det_dir)
            rc = run_ui_test(argv, max(args.per_test_timeout, 180 + args.steps))[0]
            ok, lines = det_run_report(det_dir, {"host": False, "client1": False}, configs=configs)
            if gored:
                gok, glines = det_gored_verdict(det_dir, poke)
                ok, lines = ok and gok and rc != 0, lines + glines
            else:
                ook, olines = det_orders_verdict(det_dir)
                ok, lines = ok and ook and rc == 0, lines + olines
            results.append((tag, ok, lines))
            # keep each arm's evidence: the next arm det_clear()s the shared dir
            keep = os.path.join(
                REPO,
                "tmp",
                "ui_test",
                "det_config1",
                re.sub(r"[^A-Za-z0-9]+", "_", tag).strip("_").lower(),
            )
            shutil.rmtree(keep, ignore_errors=True)
            shutil.copytree(det_dir, keep)
            lines.append("      artifacts: %s" % keep)
    return results


def run_det_standard(args, cfg):
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
        argv = build_scenario_argv(
            cfg=cfg,
            determinism=True,
            steps=args.steps,
            host="%s:%s"
            % (args.vms[0], "mp_host_start_ai.txt" if args.ai else "mp_host_start.txt"),
            clients=["%s:mp_client_start.txt" % args.vms[1]],
            connect_ip=args.vms[0],
            timeout=max(args.timeout, 120 + args.steps),
            extra_ini=[extra_ini] if extra_ini else [],
            net_extra=net_extra,
            extra_ini_host=extra_ini_host,
            extra_ini_client=extra_ini_client,
            ship_pacing=args.ship_pacing,
            ai_probe=100 if args.ai else None,
        )
        det_clear(det_dir)
        rc = run_ui_test(argv, max(args.per_test_timeout, 180 + args.steps))[0]
        ok, lines = det_run_report(det_dir, want)
        results.append((label, rc == 0 and ok, lines))

    # mp:D29 O3 -- the CONFIGURATION (1) shapes (players' build: no libmh.dll), SYMMETRIC and MIXED,
    # each with its go-red arm. See run_det_config1.
    results += run_det_config1(args, cfg) or []

    # THIRD SHAPE: the U28 start barrier at three peers. It is here rather than in the loop above
    # because it is not a promotion shape -- different scripts, a third peer, and a pass condition
    # that includes "both clients actually disagreed" on top of the hash comparison.
    print("\n" + "=" * 78)
    print("[det] SHAPE: U28 3-PEER START BARRIER")
    print("      both clients disagree with the host at Start; the barrier must override BOTH")
    print("=" * 78)
    ok3, lines3 = run_det_3peer(args, cfg)
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


# --det-local's own range. The lane numbers come from lane_alloc (fork F4H); the PORT does not,
# because peers of one match must share one port. 6620 sits inside the capture suite's own band
# (PORT_BASE + test index), which is tolerable only because --det-local is a by-hand diagnostic that
# never runs beside the suite -- the gate's det unit uses the VMs. Move it if that ever changes.
DET_LOCAL_PORT = 6620
# provision_lanes numbers from the BASE (lane_base + 1 is the first peer), so this is the block's
# base rather than its first lane. Allocated at fork F4H -- see tools/lane_alloc.py.
DET_LOCAL_LANE_BASE = lane_alloc.block("det_local")[0]


def add_args(ap):
    """The determinism arms' flags."""
    ap.add_argument(
        "--determinism",
        action="store_true",
        help="run the UI-PATH determinism check instead of the capture suite: drive both peers into the "
        "game via the real UI (match_launch scripts, no force-entry), run --steps in-game, mp_analyze -> "
        "ALL PAIRS IDENTICAL",
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
        "--det-config1-gate",
        action="store_true",
        help="--determinism: run_gate's det_c1 unit -- ONLY the SYMMETRIC CONFIGURATION (1) clean "
        "arm (both peers without libmh.dll, ALL PAIRS IDENTICAL). The MIXED shape and the go-red "
        "arms stay in --det-config1 / --det-standard.",
    )
    ap.add_argument(
        "--det-config1",
        action="store_true",
        help="--determinism: run ONLY mp:D29's CONFIGURATION (1) shapes -- SYMMETRIC (both peers "
        "without libmh.dll) and MIXED ((1) on the host vs mode=original with libmh on the client) "
        "-- each clean (ALL PAIRS IDENTICAL, both harnesses naming the configuration asked for) and "
        "with a GO-RED arm (a host-only players+0x10 poke at steps/2, capped 1500, must red the pair "
        "at exactly that step in exactly that region). VM pair by default; --det-local runs them on "
        "two lanes of this box. Included automatically in --det-standard.",
    )
    ap.add_argument(
        "--u19b-quit3",
        action="store_true",
        help="--determinism: run ONLY mp:U19b's 3-PEER CLEAN QUIT shape (host on vms[0], one "
        "survivor client on vms[1], the QUITTER as a LOCAL LANE on this box -- the same topology "
        "and the same shared lane as --det-3peer, so the two cannot run concurrently). A client "
        "ESC-quits a running 3-peer match; the two survivors must keep stepping (never seen in the "
        "log at all, an on_gameover or a B2 fast-drop is a FAIL) and mp_analyze.py over both "
        "survivors' logs must read ALL PAIRS IDENTICAL. NOT included in --det-standard.",
    )
    ap.add_argument(
        "--l1f-ping3",
        action="store_true",
        help="mp:L1f: run ONLY the 3-PEER LOBBY-PING shape (host on vms[0], one client on vms[1], "
        "the second client as a LOCAL LANE on this box -- the same topology and the same shared "
        "lane as --det-3peer, so they cannot run concurrently). Three peers sit in ONE lobby in the "
        "ship configuration; every peer's screen must show a real ping for every OTHER occupied "
        "slot, which on a CLIENT is only reachable through the host's published summary (the "
        "transport is a client-server star). Verdict from tools/check_lobby_ping.py --published "
        "--every-slot --agree. NOT a --determinism shape: the walk never leaves the lobby.",
    )
    ap.add_argument(
        "--u19j-gpfg3",
        action="store_true",
        help="mp:U19j: run ONLY the 3-PEER GONE-PEER FRAME GUARD shape in CONFIGURATION (1) (host "
        "on vms[0], a survivor on vms[1], a sim-FENCED peer as the local det3 lane -- same lane as "
        "--det-3peer, so they cannot run concurrently). The survivors drop the fenced peer after "
        "data_timeout_ms and play on; its later heartbeats reach the host's gone-peer branch, where "
        "the byte-patch carrier must log FIRED and the host must keep playing. Verdict from "
        "tools/check_gone_peer_guard.py.",
    )
    ap.add_argument(
        "--u19j-gpfg3-unguarded",
        action="store_true",
        help="mp:U19j's NEGATIVE twin: the same shape with [net] gone_peer_frame_guard=0, EXPECT "
        "RED -- XFAIL (exit 0) only when the host went red on the garbled frame (outcome 7 raised "
        "inside lockstep after the gone side's frame); a clean run is XPASS and fails.",
    )
    ap.add_argument(
        "--x2a-map-variant",
        action="store_true",
        help="mp:X2a: run ONLY the open-redirect rig proof on the two independent VMs (host on "
        "vms[0], client on vms[1]) -- the client's OWN blue monday.mpm is genuinely mutated "
        "(tools/map_variant.py's one-bit flip, backed up and restored around the run), so the "
        "download + open-redirect run for real instead of through the local-lane "
        "map_test_pretend knob. Verdict from tools/check_map_redirect.py.",
    )
    ap.add_argument(
        "--det-standard",
        action="store_true",
        help="--determinism: run the standard shapes and report them SEPARATELY, because a "
        "merged verdict hides WHICH shape failed -- SYMMETRIC at ship config (promotion on both "
        "peers), mp:D29's two CONFIGURATION (1) shapes (SYMMETRIC without libmh.dll, and MIXED (1) "
        "vs mode=original -- each with a go-red arm; see --det-config1), the U28 "
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
    ap.add_argument(
        "--shim-delay",
        type=float,
        default=0.0,
        metavar="MS",
        help="mp:TL-SHIMUDP -- run --determinism's peers through tools/net_shim.py at this ONE-WAY "
        "delay in ms (rtt = 2x). Before this the shim needed a hand-started net_shim.py pointed at "
        "the host and a manually-widened --timeout-frames; this makes `--determinism --shim-delay N` "
        "(with --transport udp, if that is the run) a single command -- the shim is started "
        "targeting the host VM, the clients are pointed at it instead, and the [uitest] frame budget "
        "is scaled with the delay (mp:T3d: a shimmed run's `peers 1` connect step needs more real "
        "time than the LAN-tuned default). VM-topology --determinism only (0 = off, the default; "
        "not supported with --det-local this wave).",
    )
    ap.add_argument(
        "--shim-jitter",
        type=float,
        default=0.0,
        metavar="MS",
        help="+/- uniform ms around --shim-delay (mp:TL-SHIMUDP; meaningless without --shim-delay).",
    )
    ap.add_argument(
        "--det-local",
        action="store_true",
        help="run --determinism with BOTH peers on this box (local lanes) instead of the VM pair. "
        "Faster and needs no VMs, but a WEAKER test: same CPU, same libraries, same FP environment, "
        "so a divergence that only appears across machines cannot show up. For iteration; keep the "
        "VM pair as the gate.",
    )


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    add_args(ap)
    add_runner_args(ap)
    add_net_args(ap)
    add_steps_arg(ap)
    add_extra_ini_arg(ap)
    add_harness_extra_arg(ap)
    return ap


def main(argv=None, lenient=False):
    ap = build_parser()
    args = parse_mode_args(ap, argv, lenient)
    rc = apply_net_args(ap, args, peers_local=args.det_local if args.determinism else True)
    if rc:
        return rc
    cfg = RunnerConfig.from_args(args)
    print_desktop_banner(cfg)
    if args.det_selftest:
        return det_standard_selftest()
    if args.sp_determinism:
        return run_sp_determinism(args, cfg)
    # mp:L1f -- a LOBBY shape, so it is dispatched before the determinism block rather than inside
    # it: there is no sim in this walk to compare and no harness to arm.
    if args.l1f_ping3:
        ok, lines = run_l1f_ping3(args, cfg)
        print("\n".join(lines))
        if ok is None:
            return 0  # VM down -> SKIP, not a failure
        print("[l1f] 3-PEER LOBBY PING: %s" % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1

    # mp:U19j -- a match shape outside --determinism (nothing is hashed; the verdict is the checker's).
    if args.u19j_gpfg3 or args.u19j_gpfg3_unguarded:
        rc_all = 0
        for guarded, on in ((True, args.u19j_gpfg3), (False, args.u19j_gpfg3_unguarded)):
            if not on:
                continue
            ok, lines = run_u19j_gpfg3(args, cfg, guarded)
            print("\n".join(lines))
            name = "GUARDED" if guarded else "UNGUARDED (expect red)"
            if ok is None:
                print("[u19j] %s: SKIP" % name)
                continue
            verdict = ("PASS" if ok else "FAIL") if guarded else ("XFAIL" if ok else "XPASS/FAIL")
            print("[u19j] 3-PEER GONE-PEER FRAME GUARD %s: %s" % (name, verdict))
            rc_all |= 0 if ok else 1
        return rc_all

    # mp:X2a -- a match shape on the two independent VMs, outside --determinism (nothing is hashed;
    # the verdict is check_map_redirect.py's).
    if args.x2a_map_variant:
        ok, lines = run_x2a_map_variant(args, cfg)
        print("\n".join(lines))
        if ok is None:
            print("[x2a] SKIP")
            return 0
        print("[x2a] OPEN-REDIRECT ON INDEPENDENT VMS: %s" % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1

    # The determinism SHAPE flags mean nothing outside --determinism, and a bare `--u19b-quit3`
    # used to fall through to the WHOLE default suite (2026-09-22: 27 rows into a 70-row run before
    # anyone noticed, holding the rig lease the while). Refuse rather than run the wrong thing.
    if not args.determinism and (
        args.det_standard
        or args.det_3peer
        or args.u19b_quit3
        or args.det_config1
        or args.det_config1_gate
    ):
        print(
            "[det] --det-standard / --det-3peer / --u19b-quit3 are --determinism shapes: pass "
            "--determinism with them (without it the default capture suite would run instead)"
        )
        return 2

    if args.determinism:
        return run_determinism(args, cfg)
    ap.print_usage()
    print(
        "det_arms.py: pick a mode (--determinism [--det-standard/--det-config1/--det-3peer/"
        "--u19b-quit3], --sp-determinism, --det-selftest, --l1f-ping3, --u19j-gpfg3, "
        "--x2a-map-variant)"
    )
    return 2


def run_determinism(args, cfg):
    """--determinism: the 2-peer UI-path lockstep check, or one of its named shapes."""
    # C7: promotion is TWO runs, not one. --det-standard runs both shapes and reports them apart.
    if args.det_standard:
        return run_det_standard(args, cfg)
    if args.det_config1 or args.det_config1_gate:
        res = run_det_config1(args, cfg, local=args.det_local, gate=args.det_config1_gate)
        if res is None:
            return 0  # VM down -> SKIP, not a failure
        print("\n" + "=" * 78)
        print("[det] CONFIGURATION (1) SHAPES -- one verdict each")
        print("=" * 78)
        for label, ok, lines in res:
            print("  %-44s %s" % (label, "PASS" if ok else "FAIL"))
            for ln in lines:
                print(ln)
        return 0 if all(ok for _, ok, _ in res) else 1
    if args.det_3peer:
        ok, lines = run_det_3peer(args, cfg)
        print("\n".join(lines))
        if ok is None:
            return 0  # VM down -> SKIP, not a failure
        print("[det] U28 3-PEER BARRIER: %s" % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    if args.u19b_quit3:
        ok, lines = run_u19b_quit3(args, cfg)
        print("\n".join(lines))
        if ok is None:
            return 0  # VM down -> SKIP, not a failure
        print("[det] U19b 3-PEER CLEAN QUIT: %s" % ("PASS" if ok else "FAIL"))
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
            stock_exe=cfg.stock_exe,
        )
        if not plan:
            return 1
        port, _, names = plan["determinism"]
        print("[det] LOCAL pair on this box -- weaker than the VM pair (shared CPU/libs/FP env)")
        where = dict(
            host="lane=%s:%s" % (names[0], host_script),
            connect_ip="127.0.0.1",
            port=port,
            clients=["lane=%s:mp_client_start.txt" % names[1]],
        )
    else:
        where = dict(
            host="%s:%s" % (args.vms[0], host_script),
            clients=["%s:mp_client_start.txt" % args.vms[1]],
            connect_ip=args.vms[0],
            # TL-HARN17: the host's `peers 1` wait is counted in FRAMES; 240 s at the blit floor.
            timeout_frames=frames_for_seconds(240, BLIT_LOCAL_FPS_FLOOR),
        )
    shim = None
    if args.shim_delay:
        # mp:TL-SHIMUDP -- VM pair only: a local pair shares this box's ports with the shim.
        if args.det_local:
            print(
                "[det] --shim-delay needs the VM pair -- not supported with --det-local this "
                "wave (mp:TL-SHIMUDP residue)"
            )
            return 2
        shim = {"target": args.vms[0], "delay": args.shim_delay, "jitter": args.shim_jitter}
        # mp:T3d -- a shimmed `peers 1` connect needs a frame budget linear in the delay.
        where["timeout_frames"] = 6000 + int(args.shim_delay) * 40
    argv = build_scenario_argv(
        cfg=cfg,
        determinism=True,
        steps=args.steps,
        timeout=max(args.timeout, 120 + args.steps),
        shim=shim,
        ship_pacing=args.ship_pacing,
        extra_ini=args.extra_ini,
        extra_ini_host=args.extra_ini_host,
        harness_extra_host=args.harness_extra_host,
        # mp:R2 -- the symmetric knob (e.g. D16's late synth_at) every peer must share
        harness_extra=args.harness_extra,
        # overrides a pinned [net] key in place (make_ini merges by key)
        net_extra=args.net_extra,
        # mp:P14 (5) -- --relay-shim-delay's client-only relay= override
        net_extra_client=args.net_extra_client,
        # D10: an AI run must SHOW the AI running
        ai_probe=100 if args.ai else None,
        **where,
    )
    if not args.det_local:
        down = [ip for ip in args.vms[:2] if not vm_reachable(ip)]
        if down:
            print("determinism SKIP -- VM(s) unreachable: %s" % ", ".join(down))
            return 0
    return run_ui_test(argv, max(args.per_test_timeout, 180 + args.steps))[0]


if __name__ == "__main__":
    import hostlock

    if set(sys.argv[1:]) & {"--det-selftest"}:  # offline: must not wait on the rig lease
        raise SystemExit(main())
    raise SystemExit(hostlock.run_rig_tool(main, "det_arms"))
