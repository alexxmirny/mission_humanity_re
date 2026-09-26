#!/usr/bin/env python3
"""tools/ui_test.py -- UI regression runner for the [uitest] script interpreter (Phase 4 of
the UI-testing plan).

Launches the game with a chosen UI-STATE script (ui_drive.cpp interpreter), waits for the script to
report `; [script] COMPLETE` (or `TIMEOUT`) in mh_uidrive.log, pulls the per-screen captures the script
took (capture_<name>.bmp -> PNG), and diffs each against a committed baseline -> per-capture pass/fail.

Single-peer (local host, no network):
    python tools/ui_test.py mp_menu_walk.txt
    python tools/ui_test.py mp_menu_walk.txt --update-baselines   # (re)generate the baselines

Multi-peer (reuses mp_run's ssh/scp plumbing). The host may be the local dev box OR a VM (`ip:script`);
clients are VMs. The host launches first; once it is LISTENING (its lobby is up) each client is pointed at
the host by rewriting its setup.dat server IP (setup_dat.py -- no keyboard typing) and launched, then every
peer is waited on concurrently (a host that gates on `peers N` before Start only finishes AFTER the clients
join, so we don't wait for host COMPLETE up front):
    python tools/ui_test.py --host mp_host_start.txt --client <peerA>:mp_client_start.txt
    # both peers on VMs -> the dev box stays free (e.g. for a parallel mp_run lockstep run):
    python tools/ui_test.py --host <peerA>:mp_host_start.txt --client <peerB>:mp_client_start.txt
    python tools/ui_test.py --host mp_menu_walk.txt \
        --client <peerA>:mp_client_join.txt --client <peerB>:mp_client_join.txt

Baselines live in tools/uiscripts/baselines/<peer-label>/<capture>.png and are committed. The diff is
perceptual (fraction of pixels whose max-channel delta exceeds --pixdelta); a capture PASSES when that
fraction is <= --tol. This tolerates the mouse-cursor sprite / minor AA while catching layout changes.

Rig facts (see the UI-input notes): the DLL present hook -- which capture AND the interpreter piggyback --
only installs with the FULL [net] lockstep block (written here); a Hyper-V VM presents frames headless
(no vmconnect needed). VM peers run the run-without-focus mh.focus.exe via an interactive scheduled task.
"""

import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

import desktop  # --desktop: CreateProcessW onto an isolated desktop object
import lane_alloc  # TL-LANECOLLIDE: this tree's own solo lane, for the 'solo=<name>' peer spec
import machine_config as machine  # LAN/game/VM defaults (bootstrap E2)
import make_lane  # LANE_ROOT, for the 'lane=<name>' / 'solo=<name>' peer specs
import mp_run  # reuse ssh/scp/ps/sh (the proven VM plumbing)
import setup_dat  # edit a client's setup.dat server IP (point it at the host without typing)
import win_job  # TL-SUITE-TEARDOWN: kill-on-close job assignment, shared with desktop.py/test_ui.py
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL = os.path.join(REPO, "src", "mh_dll", "Release", "mh.dll")
# --dll overrides it for ONE run (tools/coverage.py needs the unoptimised build). A module-level
# constant read at every launch site is exactly what made the Release binary un-overridable.
DLL_OVERRIDE = ""


def g_dll():
    """The mh.dll this run deploys: --dll if given, else the Release build."""
    return DLL_OVERRIDE or DLL


# The msvfw32 proxy shim. With --stock-exe a peer runs RETAIL bytes and gets
# mh.dll through this instead of through an added import descriptor. The lane/peer file keeps the
# name mh.focus.exe either way, so launchers, taskkill and crash_report's image-name attribution are
# untouched -- only the exe's CONTENT changes.
SHIM = os.path.join(REPO, "src", "mh_dll", "Release", "msvfw32.dll")
# SATELLITE DLLs (fork F4B): siblings mh.dll LoadLibrary()s beside itself. They are build outputs
# exactly like mh.dll, so a run must not leave a peer holding a stale one -- see refresh_satellites()
# for why they are refreshed rather than deployed, and deploy_peer_satellites() for the VM half.
# make_lane.DEFAULT_SATELLITES is the authority on which lanes GET one, and since fork F4D this is
# DERIVED from it rather than repeated: the two were always required to be the same names, and a
# second satellite (libmh.dll, the spine) is where "required to be the same" stops holding by
# discipline. A peer left without libmh.dll runs configuration (1) -- it boots, plays, and passes
# most things, which is precisely why a drifted copy here would not announce itself.
SATELLITES = list(make_lane.DEFAULT_SATELLITES)
# Which exe a peer was last staged with, so a run only re-pushes ~2.9 MB when the mode actually
# changed. Without this, switching modes leaves the previous mode's exe in place and the run silently
# measures the wrong thing -- the same class of staleness the fatal mh.dll copy below exists to stop.
PEER_EXE_MARKER = "mh_exe_mode.txt"
UISCRIPTS = os.path.join(REPO, "tools", "uiscripts")
BASELINES = os.path.join(UISCRIPTS, "baselines")
COMMITTED_SAVES = os.path.join(UISCRIPTS, "saves")


def resolve_save(name, machine_dir):
    """Resolve a scenario save by NAME (no extension): the committed copy under
    tools/uiscripts/saves/ wins; a save not committed there still comes from the machine dir.
    Fork F1D -- the two scenario saves (11.sav, ayy30.sav) must stage from the repo so a clean
    clone runs their scenarios without a machine install; the machine fallback keeps every other
    save (the 137-entry SAVE_STORAGE index, ad-hoc polygon saves) working unchanged."""
    committed = os.path.join(COMMITTED_SAVES, name + ".sav")
    if os.path.isfile(committed):
        return committed
    return os.path.join(machine_dir, name + ".sav")


# The full [net] block the present hook needs (a minimal block silently disarms capture + the interpreter).
NET_BLOCK = """[net]
host=0.0.0.0
port=6501
host_assign=1
log=1
lockstep_step_ms=30
lockstep_step_eps_ms=0.01
sim_step_ms=10
rx_spin=0
horizon_heartbeat_ms=50
defang_overlay=0
log_gamemode=0
game_speed_pct=0
eager_advertise=1
hires_clock=1
qpc_clock=1
lockstep_log=1
bootstrap=1
"""
# mp:SES5 decision (5): the DLL's compiled default for [trace] temporal flipped to 0 for the SHIPPED
# ini (a profiling instrument nobody has needed to answer a player report) -- the rig/lane inis keep
# it 1 explicitly, since UI-REC / determinism / pacing work still wants the per-event trace. Emitted
# by make_ini AFTER the --net-extra lines, never inside NET_BLOCK: the extras are appended to the
# [net] text, so a section header at NET_BLOCK's tail would put `module=none` / `transport=tcp` /
# `force_relay=1` under [trace] and every network row would run with its override silently ignored
# (the 2026-09-21 wave-1 suite: 34 reds, all of them this).
TRACE_BLOCK = "\n[trace]\ntemporal=1\n"


# Overridable via --defang. THE DEFAULT IS 0 = THE SHIP (mh_net.example.ini), since
# tooling:TL-RIG-DEFANG, 2026-09-22. It was 1 for two years and that was the rig lying to us: 1
# NOPs the mode-8 store at 0x004c85ed (defang_xui) -- the wait-screen frame that ENDS a resync
# barrier AND the leader's only clear of RESYNC_IN_PROGRESS -- so on a rig lane the one-shot
# latched after the first force_resync and every later fire was a silent no-op. The instrument
# (mode-8 frames) and the mechanism it watched were NOP'd by the same knob, which is how a barrier
# storm firing once every 2 s in the field read as "0 mode-8 frames" on a green suite for two
# months (dead-ends G274). Pass `--defang 1` only to reproduce that blindness deliberately.
# resync_wait_fix stays on so a defang-off run does not perma-hang.
DEFANG_OVERLAY = 0
# Extra [net] lines (';'-separated k=v), e.g. the per-group overlay knobs "defang_xui=1;defang_tt_wait=2".
EXTRA_NET = ""
# mp:R7a -- extra [net] lines for the CLIENT peers ONLY (--net-extra-client). See make_ini.
CLIENT_NET_EXTRA = ""
# Which transport this run asked for -- `[net] transport`, resolved ONCE from --net-extra (mp:T1).
# DEFAULT udp SINCE 2026-09-20 (user ruling): the DLL's own compiled default flipped from tcp to udp
# and the lane ini (NET_BLOCK) carries no `transport=` line, so this default MUST equal the DLL's --
# the readiness probe below is keyed by it, and a harness that assumed tcp against a lane that bound
# mh_net_udp.dll would report a healthy host as never-ready. A scenario that needs TCP pins
# `transport=tcp` in its net_extra (test_ui.py rows); nothing relies on the default being tcp.
# It is a harness-visible fact and not merely a game setting, because the "host is ready" probe is
# transport-specific: a udp host never appears in a TCP listener table, so a probe that assumes tcp
# reports a perfectly healthy host as never-ready and the run aborts naming the GAME's state. That
# is exactly the 2026-07-27 outage in remote_listening's docstring, one layer up, which is why the
# transport is resolved here rather than sniffed at each probe: one place to be wrong, and it is
# printed. --extra-ini cannot carry it (a fragment's [net] is REFUSED in main), so --net-extra is
# the whole channel and this is complete.
RUN_TRANSPORT = "udp"
# net_shim.py's control-port default (DEFAULT_CONTROL_PORT there) -- and --shim-control-port's.
# The control port is a machine-wide TCP singleton, so two shims on one box need two of them:
# test_ui.py hands every local shim row its OWN port (test_ui.shim_control_port, the 6900 band,
# derived from the row's shim port like the game port is from its index; lane_alloc --check proves
# no two rows collide). Until 2026-09-24 shim_start never forwarded `--control`, every shim listened
# HERE, and the suite had to fold all shim rows into one serial worker (1536 s of a 1731 s suite).
# A hand run / VM-topology run that passes nothing still gets this default.
SHIM_CONTROL_PORT = 6699
# Whole extra INI SECTIONS appended verbatim after the standard blocks (--extra-ini FILE). This is how a
# test opts into a feature that ships OFF: the debug overlay's [debug] block (tools/uiscripts/ini/) is
# enabled only for the overlay regression test, so every other baseline keeps rendering an overlay-free
# frame. Keep such a block's readouts to values that do NOT vary run-to-run, or the baseline will flap.
EXTRA_INI = ""
# --extra-ini-host: an ini fragment for the HOST peer ONLY. The asymmetric twin of --extra-ini, and
# it exists for the same reason harness_extra_host_lines() does -- to make the two peers DIFFER on
# purpose. O3's promotion test is the case it was built for: run the original container on one peer
# and the reimplemented one on the other, and let the lockstep hash referee. A symmetric promoted run
# cannot do that job at all: determinism only proves the two peers agree with EACH OTHER, so a
# deterministic-but-wrong reimplementation makes both peers wrong identically and the gate goes green.
EXTRA_INI_HOST = ""
# --extra-ini-client: the same thing aimed at every NON-host peer, and it is what actually makes an
# asymmetric run asymmetric TODAY. `--extra-ini-host promote_lockstep.ini` was enough only while
# promotion was opt-in. Since C8-f made it the SHIPPING DEFAULT (`SHIP_PROMOTE_LOCKSTEP = 1` in
# mh/seams/net_internal.h), a peer that receives no `[promote]` section at all still promotes -- so
# the host-only fragment sets a key that was already 1 and both peers run OURS. The shape did not
# pass while doing it (det_run_report's liveness check refuses a run whose asymmetry evaporated), so
# nothing was ever waved through; it simply became UNRUNNABLE and stayed that way, reported as a
# FAIL on every --det-standard invocation. Making the original the reference oracle now takes an
# explicit `[promote] lockstep=0` on the CLIENT, which is what this flag delivers.
EXTRA_INI_CLIENT = ""


def make_harness_ini(steps, is_host=False):
    """The [harness] block for the UI-path determinism run, merged into the lane's mh_net.ini.

    ONE FILE SINCE FORK F2G, AND AN EXPLICIT ARM. The block used to be deployed as its own
    `mh_harness.ini` next to the exe, because that was the only file harness.cpp read AND its very
    existence was what armed the harness. Both halves are gone: the DLL reads [harness] out of
    mh_net.ini, and it arms only on `enable=1` below. A stale copy of the old file beside the exe is
    REFUSED by the DLL now rather than ignored -- which is the direct answer to what this docstring
    used to warn about, an 800-step gate that actually ran the host to 200 and the client to 3000
    because whatever mh_harness.ini happened to be sitting on each VM decided the real stop_step.

    `enable=1` LEADS THE BLOCK, and it is emitted here rather than by the caller so that the decision
    "this run is instrumented" and the config that instruments it cannot be written apart. The merge
    into the lane ini is BY SECTION (ini_merge_fragment), because an appended second [harness] block
    would be present and unreachable -- the trap this module's merge helpers exist for, now one file
    closer to everything else.

    seed_mode=2 -> both peers reseed identically at sim step 0 (same as mp_run), so any divergence is a
    real desync. NO force-entry: the [net] block deliberately omits mp_players/player_id/mp_map.

    D6: the moving-unit workload rides along here too, and for the same reason it exists at all -- a
    determinism run over an IDLE world compares a world in which nothing happens, so a transient
    divergence re-converges for free. ONE seed is drawn per run (module-level SYNTH_SEED) and given to
    every peer, so the peers agree while the value stays fresh per run.
    """
    base = (
        (mp_run.HARNESS % (steps, ORDER_MODE, ORDER_LOG))
        + (mp_run.SYNTH % (SYNTH_MOVE, SYNTH_SEED, SYNTH_AT, SYNTH_EVERY))
        + ("ai_probe_step=%d\n" % AI_PROBE_STEP if AI_PROBE_STEP else "")
    )
    return harness_apply_extras(
        base, harness_extra_lines() + (harness_extra_host_lines() if is_host else "")
    )


def wants_harness(harness_steps, is_host):
    """Does this launch arm the harness at all? The ONE answer, so the two launch paths agree.

    They did not before F2G: local_launch deployed the harness file on `steps>0 or HARNESS_EXTRA`
    while remote_launch deployed it on `steps>0` alone, so `--harness-extra` reached a local peer and
    was silently dropped on a VM one -- an asymmetry nobody asked for in a runner whose whole job is
    to make two peers identical except where a flag says otherwise. With the file merged there is one
    ini and one decision; this is it.

    HOST-ONLY EXTRAS ARM EVERY PEER (mp:D33, 2026-09-25). `--harness-extra-host` is meant to make the
    peers' KNOBS differ, not whether the harness runs at all. It used to arm the host alone, so a
    harness-armed host (whose order_queue_tail_clear(), since removed, zeroed the dead slots of llm_strat_order[300]
    every step) was compared against a harness-less client keeping retail's stale residue there --
    the D21 watch hashes the whole array, so it read `DESYNC step=100 first_region=41 order_queue`
    (78/79 samples) while every world region stayed identical. The client now gets the base block
    (and HARNESS_EXTRA) too; a row that arms a workload host-only must still pass synth_move=0 in
    `harness_extra` or the client's default D6 mover runs.
    """
    return bool(harness_steps > 0 or HARNESS_EXTRA or HARNESS_EXTRA_HOST)


def harness_apply_extras(base, extras):
    """Merge extra `k=v` lines into a [harness] block by KEY, overriding in place.

    NOT concatenation, and the difference is load-bearing: `GetPrivateProfileIntA` returns the FIRST
    occurrence of a key in a section, so an appended `fixed_step=0` after the template's
    `fixed_step=1` is read as 1 and the override silently does nothing. Caught while wiring
    P0-SPDET, whose whole point is knobs that turn the oracle on -- a silently-ignored pin_wallclock
    would have produced a run that looked armed and compared wall-clock garbage.

    Same failure family as the duplicate `[promote]` SECTION that cost a C6 acceptance run; this is
    the per-KEY version of it, and det_standard_selftest covers both.
    """
    over = {}
    for ln in (extras or "").splitlines():
        ln = ln.strip()
        if "=" in ln and not ln.startswith(";"):
            k, v = ln.split("=", 1)
            over[k.strip()] = v.strip()
    out, seen = [], set()
    for ln in base.splitlines():
        k = ln.split("=", 1)[0].strip() if "=" in ln and not ln.strip().startswith(";") else None
        if k in over:
            out.append("%s=%s" % (k, over[k]))
            seen.add(k)
        else:
            out.append(ln)
    for k, v in over.items():
        if k not in seen:
            out.append("%s=%s" % (k, v))
    return "\n".join(out) + "\n"


def harness_extra_lines():
    """Extra [harness] lines for EVERY peer (--harness-extra). The symmetric twin of
    harness_extra_host_lines().

    Added for P0-SPDET, whose knobs (pin_wallclock / fixed_step / region_hash_step) must reach the
    one peer a single-player run has -- and --harness-extra-host is REFUSED on a single-peer run
    precisely because there is no second peer to differ from. Symmetric by construction, so unlike
    the host-only flag it cannot accidentally create the asymmetry a determinism run is measuring.
    """
    return "".join(kv.strip() + "\n" for kv in (HARNESS_EXTRA or "").split(";") if kv.strip())


def harness_extra_host_lines():
    """Extra [harness] lines for the HOST peer ONLY (--harness-extra-host).

    Asymmetric BY DESIGN: this is how one peer is perturbed so the gate has something to find
    (D3's rng_perturb_slot, D11's region_poke). Deploying it to every peer would corrupt them
    identically and the run would come back clean -- the exact false negative these tests exist to
    avoid. mp_run.py has the same flag for the force-entry path; this is the UI-path twin.
    """
    return "".join(kv.strip() + "\n" for kv in (HARNESS_EXTRA_HOST or "").split(";") if kv.strip())


# Artifacts that exist only for SOME runs, so a missing one is not an error. mp_run.LOGNAMES is the
# always-expected set (a gap there is reported); these are pulled silently. mh_orders.bin is the
# project's SECOND lockstep oracle -- two peers whose recordings are byte-identical applied the same
# order stream -- and until 2026-07-28 it was written on the VM and never ferried back, which made it
# unusable for exactly the question it answers best.
OPTIONAL_ARTIFACTS = ["mh_orders.bin", "mh_clock.bin"]

# --record: order_mode=1 makes the harness RECORD every dispatched order to mh_orders.bin (plus the
# per-step clock track mh_clock.bin, which a real-time recording needs because fixed_step=0 means the
# clock advances by variable per-frame deltas). Both land in the per-run log folder. That turns an
# interactive session into a replayable artifact -- the point being that a bug found while a human
# plays can afterwards be reproduced without the human. order_log=1 additionally dumps the queue
# contents as ";ord" lines, which is what makes the recording readable rather than just replayable.
ORDER_MODE = 0
ORDER_LOG = 0

# D6: the synthetic moving-unit workload, ON by default for determinism runs (see make_harness_ini).
# The seed is drawn ONCE per process with os.urandom and shared by every peer -- a PRNG-derived
# destination would be identical on every peer by construction and would never exercise the wire.
SYNTH_MOVE = 1
SYNTH_SEED = mp_run.synth_seed()
SYNTH_AT = 60
SYNTH_EVERY = 1  # every step: keeps units[] changing continuously (cost measured as nil)

# D10 (--ai): cadence of the "; AIPROBE" line -- master gate, loop bound, per-player ai_enabled, and
# the AI PRNG slot. Set only for AI-active runs, because its ONLY job is to make "the AI actually ran"
# a thing you READ rather than assume. A value seen once proves nothing; a cadence shows motion.
AI_PROBE_STEP = 0

# --harness-extra-host: ';'-separated [harness] k=v written to the HOST's ini only. See
# harness_extra_host_lines() for why it must never reach the clients.
HARNESS_EXTRA_HOST = ""

# --harness-extra: ';'-separated [harness] k=v written to EVERY peer's ini. See
# harness_extra_lines(); P0-SPDET's pin_wallclock/fixed_step/region_hash_step ride here.
HARNESS_EXTRA = ""


# --ship-pacing: drop the pinned pacing lines so the DLL's OWN shipping defaults apply (lookahead
# 100 ms + the adaptive controller, sim sub-step 20 ms). The rig has always pinned 30/10, which was
# right while those values lived only here -- but it means a green determinism gate says nothing
# about what a player actually runs. Explicit rather than default, so the pixel baselines keep their
# established timing.
SHIP_PACING = False
# --headless: [video] no_present=1 cuts the DirectDraw blit inside llm_gfx_present_flip. Frames are
# still COMPOSED in software, so captures are byte-identical (A/B-verified 2026-07-28); only the push
# to screen is gone. Lets several instances share a machine without fighting over the display.
# CORRECTNESS RUNS ONLY -- no blit means no vsync wait, which is exactly what makes it wrong for
# pacing measurement. The parallel-lane notes.
HEADLESS = False
# --desktop <name>: run the game on its own Windows desktop object. Empty = the interactive desktop,
# i.e. today's behaviour. This is the mechanism-INDEPENDENT answer to the window that headless cannot
# keep unmapped -- see tools/desktop.py for what was ruled out before reaching for it.
DESKTOP = ""
# --launch-args / --deploy-save: the two knobs a scenario needs to be driven into a mode the MENU
# cannot reach. Tactical mode is the case that forced them: a mission is entered from a played
# strategic game (a live planet, a seated control group, an enemy base in range), so no menu walk
# reaches one, and the DLL's own `--tactical <save>` verb synthesises the squad blackboard instead
# (seams/launch.cpp enter_tactical). That verb takes a save NAME and loads `save\<name>.sav`
# relative to the exe, and a lane is a fresh folder with no save dir -- hence the second knob.
# Both are empty for every other scenario, so nothing else changes shape.
LAUNCH_ARGS = ""
DEPLOY_SAVE = ""
# rx_spin joined this list on 2026-07-26, when P5 made it a SHIP default: the rig pins it to 0, so
# without dropping it here --ship-pacing would keep testing the pre-P5 configuration.
PINNED_PACING = ("lockstep_step_ms=30\n", "sim_step_ms=10\n", "rx_spin=0\n")


def ini_merge_section(text, section, lines):
    """Append `lines` to `[section]` inside `text`, creating the section if it is not there.

    Windows' GetPrivateProfile* reads only the FIRST section with a given name, so emitting a second
    `[video]` block does not add keys -- it writes dead text and leaves the caller believing both sets
    applied. Anything that composes an ini from independent fragments has to merge by section.
    """
    head = "[%s]" % section.lower()
    out, seen, i = [], False, 0
    for ln in text.splitlines():
        out.append(ln)
        if not seen and ln.strip().lower() == head:
            seen = True
            i = len(out)  # insert point: immediately under the header
    if not seen:
        return (
            (text.rstrip("\n") + "\n\n" if text.strip() else "")
            + head
            + "\n"
            + "".join(ln + "\n" for ln in lines)
        )
    # THE "ALREADY SET" TEST IS SCOPED TO THIS SECTION, and until fork F2G it was not: it asked
    # whether ANY line anywhere in the text already used the key, so a key name shared by two
    # sections made the merge silently drop the one being merged in. Harmless while the colliding
    # names were section-private -- and immediately fatal once D12 put every section in ONE file and
    # made `[harness] enable=1` the harness's arming signal, because `[net] enable=1` sits above it
    # in every lane ini ever written. The failure is the worst available shape: the merge reports
    # nothing, the file looks right to a reader who is not modelling GetPrivateProfile*, and the run
    # comes back green with the instrument it was supposed to arm never armed. An ini key belongs to
    # its section; a comparison that forgets that is not modelling the reader.
    end = len(out)
    for j in range(i, len(out)):
        t = out[j].strip()
        if t.startswith("[") and t.endswith("]"):
            end = j
            break
    own = {o.split("=", 1)[0].strip().lower() for o in out[i:end] if "=" in o}
    for ln in lines:  # skip keys the fragment already sets -- the fragment is the more specific one
        key = ln.split("=", 1)[0].strip().lower()
        if key in own:
            continue
        own.add(key)
        out.insert(i, ln)
        i += 1
        end += 1
    return "".join(ln + "\n" for ln in out)


def ini_split_sections(fragment):
    """[(section, [key=value, ...]), ...] for an ini fragment. Comments and blanks are dropped."""
    out, cur = [], None
    for raw in fragment.splitlines():
        ln = raw.strip()
        if not ln or ln.startswith(";") or ln.startswith("#"):
            continue
        if ln.startswith("[") and ln.endswith("]"):
            cur = (ln[1:-1].strip(), [])
            out.append(cur)
        elif cur is not None and "=" in ln:
            cur[1].append(ln)
    return out


def ini_merge_fragment(text, fragment):
    """Merge every section of `fragment` into `text`, section by section.

    CONCATENATION IS NOT COMPOSITION, and this cost a C6 acceptance run on 2026-07-29. `--extra-ini`
    and `--extra-ini-host` used to be simply appended one after the other. Give them a section in
    common -- two fragments can carry the same section -- and the host
    ini ends up with TWO `[promote]` blocks. GetPrivateProfile* reads only the FIRST, so the host-only
    keys were dead text: the run came back ALL PAIRS IDENTICAL with the asymmetry never installed.
    Exactly the trap `ini_merge_section` was written for, one caller further out.
    """
    for section, lines in ini_split_sections(fragment):
        text = ini_merge_section(text, section, lines)
    return text


def _refuse_net_fragment(text, path, flag):
    """sys.exit if `text` (an ini fragment read from `path`) carries a [net] section.

    Shared by --extra-ini, --extra-ini-host and --extra-ini-client (tooling:TL-SUITE-INIMERGE):
    make_ini always builds [net] itself from NET_BLOCK/--net-extra/--net-extra-client, so a
    fragment's own [net] can only ever become a shadowed second section -- dead-ends G69/G181. The
    general fragment refused this since 2026-08-27; the host/client ones did not, until this closed
    the gap (tools/uiscripts/ini/u28_off.ini is an existing fragment that would have hit it).
    """
    for ln in text.splitlines():
        if ln.split(";", 1)[0].strip().lower() == "[net]":
            sys.exit(
                "REFUSED: %s carries a [net] section. make_ini builds [net] itself and appends this\n"
                "fragment afterwards, and GetPrivateProfile* reads only the FIRST section of a given\n"
                "name -- so those keys would be read by nothing and the run would report success\n"
                "about a config it never had.\n"
                'Use %s "key=value;key=value" through the matching --net-extra channel instead.'
                % (
                    path,
                    "--net-extra"
                    if flag == "--extra-ini"
                    else ("--net-extra-client" if flag == "--extra-ini-client" else "--net-extra"),
                )
            )


def ini_effective(text, section, key):
    """What GetPrivateProfile* would read: FIRST section with that name, FIRST key in it. None if absent.

    Deliberately models the quirk rather than parsing sanely, because the quirk is what the guard below
    has to catch -- a key that is present in the file and unreachable by the game.
    """
    want, in_section = "[%s]" % section.lower(), False
    seen_section = False
    for raw in text.splitlines():
        ln = raw.strip()
        if ln.startswith("[") and ln.endswith("]"):
            if in_section:
                return None  # left the first matching section without finding the key
            if ln.lower() == want:
                if seen_section:
                    return None  # a LATER duplicate: unreachable
                in_section, seen_section = True, True
            continue
        if in_section and "=" in ln and not ln.startswith((";", "#")):
            k, _, v = ln.partition("=")
            if k.strip().lower() == key.strip().lower():
                return v.strip()
    return None


def resolve_transport(net_extra):
    """`[net] transport` out of a --net-extra string, defaulting to the shipping udp (2026-09-20).

    REFUSES an unknown value instead of falling back, deliberately mirroring what mh.dll itself does
    with the same key (module_bind.cpp transport_file -> mh_config_refused.log). A harness that
    quietly ran tcp for `--net-extra transport=udo` would produce a green determinism verdict for a
    transport nobody exercised -- the same class of result as the three "ALL PAIRS IDENTICAL" runs
    of 2026-08-27 that all ran the default config.
    """
    got = "udp"
    for kv in (net_extra or "").split(";"):
        k, _, v = kv.partition("=")
        if k.strip().lower() == "transport" and v.strip():
            got = v.strip().lower()
    if got not in ("tcp", "udp"):
        sys.exit(
            "REFUSED: --net-extra transport=%s is not a transport this build implements.\n"
            "The values are `udp` (the shipping default -- mh_net_udp.dll) and `tcp` (mh_net.dll).\n"
            "mh.dll would refuse this run too; refusing here as well keeps the harness from\n"
            "reporting a verdict about a transport it never ran." % got
        )
    return got


class IniLayers:
    """Accumulate an ini by (section, key), one precedence step at a time (tooling:TL-SUITE-INIMERGE).

    Each `add`/`add_lines`/`add_fragment`/`unset` call is a HIGHER-precedence step than every call
    before it on the same instance: a later call's value for an already-seen (section, key) simply
    replaces the earlier one. POSITION -- both which section comes first and which key comes first
    within a section -- is fixed by FIRST mention instead, so a later step overriding an earlier
    key's value does not relocate it; this is what keeps the rendered file close to the shape it had
    before a value changed under it. `unset` records a real "no value" (the key is dropped from the
    render entirely, e.g. so a compiled DLL default applies) rather than merely not mentioning it --
    a later step CAN still re-add it, exactly like a real override would.

    This is the single place make_ini's composition happens now, replacing the scattered
    append-then-strip-the-shadowed-line logic that used to decide precedence ad hoc at each call
    site (one instance of the bug per call site: dead-ends G69, G96, G178, G181, G249, G266, G267).
    """

    def __init__(self):
        self._order = []  # [section_lower, ...] first-seen order
        self._case = {}  # section_lower -> original-case header text
        self._keys = {}  # section_lower -> [key_lower, ...] first-seen order
        self._kcase = {}  # (section_lower, key_lower) -> original-case key text
        self._vals = {}  # (section_lower, key_lower) -> value, or None if unset

    def add(self, section, key, value):
        sl, kl = section.strip().lower(), key.strip().lower()
        if sl not in self._case:
            self._order.append(sl)
            self._case[sl] = section.strip()
            self._keys[sl] = []
        if kl not in self._keys[sl]:
            self._keys[sl].append(kl)
            self._kcase[(sl, kl)] = key.strip()
        self._vals[(sl, kl)] = None if value is None else str(value)

    def add_lines(self, section, lines):
        """`lines`: an iterable of 'key=value' strings (no section header, no bare comments)."""
        for ln in lines:
            ln = ln.strip()
            if not ln or ln.startswith((";", "#")) or "=" not in ln:
                continue
            k, v = ln.split("=", 1)
            self.add(section, k, v.strip())

    def add_fragment(self, text):
        """A whole ini fragment (one or more `[section]` blocks) as ONE precedence step."""
        for section, lines in ini_split_sections(text):
            self.add_lines(section, lines)

    def unset(self, section, key):
        self.add(section, key, None)

    def pairs(self):
        """Every (section_lower, key_lower, value) ever added, value None if unset -- what a caller
        asked this composition for, used by make_ini's round-trip self-check."""
        for sl in self._order:
            for kl in self._keys[sl]:
                yield sl, kl, self._vals[(sl, kl)]

    def render(self):
        out = []
        for sl in self._order:
            body = [
                "%s=%s" % (self._kcase[(sl, kl)], self._vals[(sl, kl)])
                for kl in self._keys[sl]
                if self._vals[(sl, kl)] is not None
            ]
            if not body:
                continue  # every key this section ever had was unset -- nothing to emit
            out.append("[%s]" % self._case[sl])
            out.extend(body)
            out.append("")
        return "\n".join(out) + "\n"


def _ini_kv_pairs(extra):
    """[(key, value), ...] from a ';'-separated `k=v;k=v` string (--net-extra's own syntax)."""
    out = []
    for kv in (extra or "").split(";"):
        kv = kv.strip()
        if kv and "=" in kv:
            k, v = kv.split("=", 1)
            out.append((k.strip(), v.strip()))
    return out


def make_ini(script_name, timeout_frames, harness_steps=0, is_host=False, ident=None):
    """Compose this peer's mh_net.ini through ONE explicit, ascending precedence order (a LATER
    layer wins a (section, key) collision -- see IniLayers), lowest to highest:

      1. defaults    -- NET_BLOCK, with DEFANG_OVERLAY baked in as [net] defang_overlay's default and
                        (if SHIP_PACING) the three PINNED_PACING keys unset so the DLL's own shipping
                        default applies; plus TRACE_BLOCK, [capture] every=0, and the [uitest]
                        identity block (enable/script/dump_screens/timeout_frames/lane).
      2. net_extra   -- --net-extra, [net] only.
      3. client_net_extra -- --net-extra-client, CLIENT PEER ONLY: overrides #2 on the client
                        (mp:R7a; dead-ends G267).
      4. lane_port   -- the lane allocator's [net] port. Wins even over an explicit --net-extra
                        port=, because two peers landing on one port is a rig fault no flag should be
                        able to cause.
      5. headless    -- --headless/--desktop's [video] no_present=1 + no_window=1.
      6. extra_ini   -- --extra-ini fragment(s) (main() already merges more than one together, in
                        the order given). [net] is REFUSED before this ever runs (main()).
      7. extra_ini_peer -- --extra-ini-host or --extra-ini-client, whichever matches THIS peer: the
                        more specific fragment, so it wins over the general one (dead-ends G178's
                        "the fragment is the more specific section" carried one level further out).
                        [net] is refused here too now, same as #6.
      8. harness     -- [harness], only if wants_harness(): the dedicated arming channel (--harness/
                        --harness-extra/--harness-extra-host, all already resolved by
                        make_harness_ini/harness_apply_extras), so it wins over anything upstream
                        that also happens to poke [harness]. This used to be merged in last through
                        ini_merge_fragment's fill-only semantics, which actually means the FIRST
                        writer of a key wins -- so despite this exact comment, the harness block
                        could previously LOSE to an earlier [harness] fragment (see this lane's
                        report for the one committed fragment that could have hit it).

    Every (section, key) any layer above actually set (or unset) is re-read back out of the
    rendered text with ini_effective() -- the GetPrivateProfile first-match model -- before
    returning; a mismatch raises naming the key, so a future change to this function that
    reintroduces a shadowed section fails loudly here instead of downstream on a rig run.
    """
    ident = ident or {}
    L = IniLayers()

    # ---- 1. defaults ------------------------------------------------------------------------------
    for section, lines in ini_split_sections(NET_BLOCK):
        for ln in lines:
            k, _, v = ln.partition("=")
            k = k.strip()
            v = str(DEFANG_OVERLAY) if k == "defang_overlay" else v.strip()
            L.add(section, k, v)
    if SHIP_PACING:
        for ln in PINNED_PACING:
            L.unset("net", ln.split("=", 1)[0].strip())
    for section, lines in ini_split_sections(TRACE_BLOCK):
        L.add_lines(section, lines)
    L.add("capture", "every", "0")
    L.add("uitest", "enable", "1")
    L.add("uitest", "script", script_name)
    L.add("uitest", "dump_screens", "0")
    L.add("uitest", "timeout_frames", timeout_frames)
    if ident.get("lane"):
        L.add("uitest", "lane", ident["lane"])

    # ---- 2. net_extra -------------------------------------------------------------------------------
    for k, v in _ini_kv_pairs(EXTRA_NET):
        L.add("net", k, v)

    # ---- 3. client_net_extra (client peer only; mp:R7a, G267) --------------------------------------
    if (not is_host) and CLIENT_NET_EXTRA:
        for k, v in _ini_kv_pairs(CLIENT_NET_EXTRA):
            L.add("net", k, v)

    # ---- 4. lane_port ---------------------------------------------------------------------------
    lane_port = ident.get("port") or 0
    if lane_port:
        L.add("net", "port", lane_port)

    # ---- 5. headless (see main()'s HEADLESS `global` note for why both keys ride together) --------
    if HEADLESS or ident.get("headless"):
        L.add("video", "no_present", "1")
        L.add("video", "no_window", "1")

    # ---- 6. extra_ini -------------------------------------------------------------------------------
    if EXTRA_INI:
        L.add_fragment(EXTRA_INI)

    # ---- 7. extra_ini_peer: whichever of --extra-ini-host / --extra-ini-client matches this peer ---
    peer_extra = EXTRA_INI_HOST if is_host else EXTRA_INI_CLIENT
    if peer_extra:
        L.add_fragment(peer_extra)

    # ---- 8. harness -----------------------------------------------------------------------------
    if wants_harness(harness_steps, is_host):
        L.add_fragment(make_harness_ini(harness_steps, is_host))

    ini = L.render()

    # ---- round-trip self-check: every (section, key) any layer above touched reads back exactly
    # as asked, through the same first-match model the game itself uses. By construction this
    # class never emits a duplicate section, so this should never fire for a caller of THIS
    # function -- it exists so a future edit that breaks that invariant fails here, loudly, with
    # the key name, rather than as a green run over the wrong configuration two layers downstream.
    for sl, kl, v in L.pairs():
        got = ini_effective(ini, sl, kl)
        if got != v:
            raise SystemExit(
                "make_ini: [%s] %s did not round-trip -- composed %r, GetPrivateProfile-style read "
                "back %r. A duplicate section shadowed it; see tooling:TL-SUITE-INIMERGE."
                % (sl, kl, v, got)
            )
    return ini


def effective_config_dict(ini_text):
    """{section: {key: value}} exactly as GetPrivateProfile* would read `ini_text` -- first section
    wins, first key within it wins. Reuses ini_split_sections' own parse so this can never disagree
    with what make_ini's round-trip check (or ini_effective itself) already proved about the file."""
    out = {}
    for section, lines in ini_split_sections(ini_text):
        sec_l = section.lower()
        if sec_l in out:
            continue  # a later duplicate section is unreachable -- ini_effective's own rule
        d = {}
        for ln in lines:
            k, _, v = ln.partition("=")
            k = k.strip().lower()
            if k not in d:
                d[k] = v.strip()
        out[sec_l] = d
    return out


def _sha256_file(path):
    try:
        h = hashlib.sha256()
        with open(path, "rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None


def write_effective_config(out_path, ini_text, dll_dir=None, extra=None):
    """Write `out_path` (tooling:TL-SUITE-INIMERGE): the merged ini AS PARSED (effective_config_dict,
    not the raw text -- a shadowed duplicate section must print as absent, the way the game reads
    it), the desktop size if known, and the deployed mh.dll/satellite sha256 if `dll_dir` is given
    and the files are on disk (a few MB, milliseconds to hash -- cheap).

    BEST-EFFORT BY DESIGN: this is a diagnostic artifact, and a run must never fail because writing
    it failed (a full disk, a locked file, a VM path that does not exist yet) -- every step is
    wrapped and a failure here only prints, it never raises.
    """
    doc = {"config": effective_config_dict(ini_text)}
    try:
        doc["desktop_size"] = primary_desktop_size()
    except Exception:
        doc["desktop_size"] = None
    hashes = {}
    if dll_dir:
        for name in ["mh.dll"] + list(SATELLITES):
            p = os.path.join(dll_dir, name)
            if os.path.isfile(p):
                h = _sha256_file(p)
                if h:
                    hashes[name] = h
    doc["dll_sha256"] = hashes
    if extra:
        doc.update(extra)
    try:
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as fh:
            json.dump(doc, fh, indent=2, sort_keys=True)
    except OSError as e:
        print("  [cfg] effective_config.json: could not write %s: %s" % (out_path, e))


def bmp_to_png(bmp, png):
    Image.open(bmp).convert("RGB").save(png)


def diff_capture(actual_png, baseline_png, pixdelta, tol, ignore=None, only=None):
    """Return (passed, fraction_differing, note). A pixel 'differs' when its max-channel abs delta
    exceeds pixdelta; the capture PASSES when the differing fraction is <= tol. `ignore` is a list of
    [x, y, w, h] rectangles zeroed in BOTH images before diffing -- for machine-specific regions (e.g. the
    on-screen host IP header) that must not count as a regression. The fraction is over the WHOLE frame
    (masked pixels simply never differ), so a mask stays a small, honest deduction.

    `only` = {"rect": [x, y, w, h], "mode": "rgb" | "ink"} inverts the mask: ONLY that rectangle is
    compared and the fraction is over the rectangle. Mode "ink" first reduces both crops to a
    luminance threshold (pixel is ink or not), so the comparison is colour-blind -- for text drawn
    in a colour the game picks per run (mp:F3b: a chat line is drawn in the SENDER's faction colour,
    cyan one run and red the next, while its glyph shapes are what the capture exists to gate).
    Shape still counts: a substitute box in place of a glyph moves the ink and fails."""
    import numpy as np

    if not os.path.isfile(baseline_png):
        return False, 1.0, "no baseline"
    a = Image.open(actual_png).convert("RGB")
    b = Image.open(baseline_png).convert("RGB")
    if a.size != b.size:
        return False, 1.0, "size %s != baseline %s" % (a.size, b.size)
    if only:
        x, y, rw, rh = only["rect"]
        a = a.crop((x, y, x + rw, y + rh))
        b = b.crop((x, y, x + rw, y + rh))
        if only.get("mode") == "ink":
            a = a.convert("L").point(lambda v: 255 if v > 64 else 0)
            b = b.convert("L").point(lambda v: 255 if v > 64 else 0)
            d = np.abs(np.asarray(a, dtype=np.int16) - np.asarray(b, dtype=np.int16))
            frac = float((d > pixdelta).mean())
            return (frac <= tol), frac, "only %dx%d at %d,%d (ink)" % (rw, rh, x, y)
        ignore = None
    aa = np.asarray(a, dtype=np.int16)
    bb = np.asarray(b, dtype=np.int16)
    d = np.abs(aa - bb).max(axis=2)
    if ignore:
        h, w = d.shape
        for x, y, rw, rh in ignore:
            x0, y0 = max(0, x), max(0, y)
            x1, y1 = min(w, x + rw), min(h, y + rh)
            if x1 > x0 and y1 > y0:
                d[y0:y1, x0:x1] = 0
    frac = float((d > pixdelta).mean())
    note = "" if not ignore else "%d region(s) masked" % len(ignore)
    if only:
        note = "only %dx%d at %d,%d" % (
            only["rect"][2],
            only["rect"][3],
            only["rect"][0],
            only["rect"][1],
        )
    return (frac <= tol), frac, note


# ---- local host peer -------------------------------------------------------------------------------


def local_existing_runs(host_dir):
    return set(glob.glob(os.path.join(host_dir, "logs", "*")))


RIG_KEY = "4d48746573746b657900000000000000000000000000000000000000deadbeef\n; rig key (tools/ui_test.py)\n"


def write_rig_key(dirpath):
    """Pin the transport's pre-shared key (mh_key.txt).

    Since 2026-07-25 the transport authenticates + encrypts with this key, and a peer with no file
    MINTS ITS OWN on first launch -- so two rig peers would each invent a different key and the join
    would be refused. Writing the same one everywhere keeps multi-peer runs on the SHIPPED secure
    path (rather than testing a configuration players never use)."""
    p = os.path.join(dirpath, "mh_key.txt")
    with open(p, "w", newline="\n") as f:
        f.write(rig_key_text())
    return p


def rig_key_text():
    """The mh_key.txt every rig peer gets. `MH_RIG_KEY=open` swaps the pinned PSK for the OPEN
    (unauthenticated, plaintext) link -- the one configuration a peer needs to talk to a relay
    that runs without --key-file, which is what the VPS relay does until dist:RP5 gives it a
    deployment key (ship-plan Phase 3). Any other value is taken as the hex key itself. Read at
    call time, not import time, so the flag reaches the child ui_test.py processes test_ui.py
    spawns through the environment they inherit."""
    v = os.environ.get("MH_RIG_KEY", "").strip()
    if not v:
        return RIG_KEY
    return v + "\n; rig key override (MH_RIG_KEY)\n"


def refresh_satellites(host_dir):
    """Re-copy the satellite DLLs a lane ALREADY HAS, from the build beside the mh.dll being
    deployed. Fork F4B.

    THE `ALREADY HAS` IS THE WHOLE DESIGN, and it is what makes the absent arm survive a launch. The
    mh.dll copy one line above exists because a run must never measure a stale build; mh_net.dll is
    a build output for exactly the same reason, and a peer running last week's transport against
    this week's mh.dll would be a mismatched pair reported as a scenario failure. But copying the
    satellite UNCONDITIONALLY would silently re-deploy the file that `make_lane --omit-satellite`
    was asked to leave out -- turning the one lane whose job is to prove the missing-module
    degradation into another bound run, green for the wrong reason. So: refresh what is there,
    create nothing. make_lane decides WHETHER a lane has a satellite; this decides only that it is
    not stale."""
    src_dir = os.path.dirname(g_dll())
    for name in SATELLITES:
        dst = os.path.join(host_dir, name)
        if not os.path.isfile(dst):
            continue
        src = os.path.join(src_dir, name)
        if os.path.isfile(src):
            shutil.copy(src, dst)


def _prior_launch_pidfile(host_dir):
    return os.path.join(host_dir, ".ui_test_last_pid.json")


def _record_launch_pid(host_dir, pid):
    """TL-RIG7: remember which pid this local_launch() put into `host_dir`, so a LATER invocation
    into the same lane folder (a separate ui_test.py process -- each arm of spdet's baseline/promoted
    pair is its own process, so in-memory state like _LOCAL_PIDS does not survive between them) can
    wait for it to actually be gone before redeploying mh.dll. Uses the TL-RIG6 process-identity
    helper (_process_created_at) to record a creation-time fingerprint alongside the pid, so PID reuse
    cannot be mistaken for "still our process" on the read side."""
    try:
        with open(_prior_launch_pidfile(host_dir), "w", encoding="utf-8") as fh:
            json.dump({"pid": pid, "started": _process_created_at(pid)}, fh)
    except OSError:
        pass


def _wait_for_prior_launch_exit(host_dir, budget=15.0):
    """TL-RIG7: spdet's DLL redeploy races the previous arm's exiting process. `Stop-Process -Force`
    (local_kill) returns as soon as TerminateProcess is issued, not once Windows has actually released
    the process's mapped mh.dll -- so the NEXT arm's shutil.copy() here could land mid-teardown
    (PermissionError), or -- observed -- land on a file Windows had already let go of NAME-wise but
    whose old mapping the exiting process was still using, comparing a fresh build against a fresh
    build under two different names ("ORIGINAL vs ORIGINAL"; the verdict logic's honest refusal was
    the only thing that caught it). Waits up to `budget` seconds for the pid a PRIOR local_launch()
    into this SAME host_dir recorded to be provably gone; a marker with no matching live process (or
    none at all -- first use of this lane) returns immediately, and this never blocks on some OTHER
    lane's process."""
    try:
        with open(_prior_launch_pidfile(host_dir), "r", encoding="utf-8") as fh:
            rec = json.load(fh)
    except (OSError, ValueError):
        return
    pid, started = rec.get("pid"), rec.get("started")
    if not pid:
        return
    deadline = time.time() + budget
    waited = False
    while time.time() < deadline:
        # _process_still_running, NOT _process_created_at: OpenProcess succeeding does not mean the
        # process is still RUNNING -- retain_exit_handle() (this same file) deliberately keeps a
        # handle open on every launched pid past its exit, which would otherwise make this loop spin
        # its whole budget on an already-gone process. GetExitCodeProcess's STILL_ACTIVE is the real
        # signal; the creation-time check stays as a defence against the pid having been RECYCLED onto
        # an unrelated process in between (astronomically unlikely in this window, cheap to rule out).
        running = _process_still_running(pid)
        if running is True and started is not None:
            now_started = _process_created_at(pid)
            if now_started is not None and abs(now_started - started) >= 2.0:
                running = False  # a different process now holds this pid number -- ours is gone
        if running is not True:
            if waited:
                print(
                    "  [launch] the prior arm's process (pid %d) has exited -- redeploying mh.dll"
                    % pid
                )
            return
        waited = True
        time.sleep(0.25)
    print(
        "  [launch] WARN: pid %d (the prior arm in this lane) was still alive after %.0fs -- "
        "redeploying mh.dll anyway (TL-RIG7 -- a redeploy PermissionError past this point is the "
        "race, not a new bug)" % (pid, budget)
    )


def local_launch(host_dir, script_src, script_name, timeout_frames, harness_steps=0, is_host=False):
    # deploy current DLL + script + ini, then launch the run-without-focus exe at the menu.
    #
    # G_DLL, NOT THE MODULE CONSTANT, and this copy is why the override has to exist here at all.
    # It runs on EVERY launch and is deliberate -- it is what stops a run measuring a stale build --
    # but it also OVERWRITES whatever make_lane deployed, so a lane provisioned with `--dll <Debug>`
    # for tools/coverage.py silently got the Release binary back one line before launch. Measured
    # 2026-09-08: the committed sim coverage baseline was recorded against /O2+LTCG, reporting 655
    # files / 40,797 instrumented lines where the Debug build reports 700 / 61,889 -- i.e. inlined
    # bodies counted as never executed, the precise reading coverage.py exists to refuse.
    _wait_for_prior_launch_exit(host_dir)  # TL-RIG7 -- before we touch mh.dll in this lane
    # tooling:TL-SUITE-TEARDOWN -- _wait_for_prior_launch_exit only knows about a prior arm THIS
    # lane's own pidfile recorded; a share_lanes row whose lane-mate outlived its own verdict (the
    # peer teardown gap this item exists for) is a DIFFERENT holder this copy has never heard of, and
    # used to surface as a bare "PermissionError: [WinError 32] ..." three frames from anything that
    # says whose peer it was. copy_or_refuse names it instead (by pidfile-independent process
    # location -- see win_job.find_process_under) and REFUSES rather than raising blind.
    win_job.copy_or_refuse(g_dll(), os.path.join(host_dir, "mh.dll"), host_dir)
    _pdb = os.path.splitext(g_dll())[0] + ".pdb"
    if os.path.isfile(_pdb):
        # The collector attributes lines through the PDB; without it the report has no source at all.
        shutil.copy(_pdb, os.path.join(host_dir, "mh.pdb"))
    refresh_satellites(host_dir)
    shutil.copy(script_src, os.path.join(host_dir, script_name))
    write_rig_key(host_dir)
    # A LANE's identity must survive this rewrite. We clobber mh_net.ini wholesale, which silently
    # dropped `[uitest] lane=N` and `[video] no_present=1` -- so every lane fell back to the stock
    # "MHMutex" (breaking the single-instance separation that makes lanes work at all) and back to the
    # visible present path. Re-emit them from lane.json, which the runner never writes.
    ident = make_lane.read_identity(host_dir)
    with open(os.path.join(host_dir, "mh_net.ini"), "w", newline="\r\n") as f:
        f.write(make_ini(script_name, timeout_frames, harness_steps, is_host, ident))
    # ONE FILE (fork F2G): the [harness] block is inside the mh_net.ini written just above, gated on
    # wants_harness() -- `--harness-extra` alone still arms it, because a scenario can need a
    # [harness] KNOB without a determinism run's step budget (the tactical capture needs
    # `tact_hash_step`: arming the tactical cadence is what makes the promoted tact_frame the one
    # that runs, and without it every converted tactical body is dead code the capture cannot see).
    #
    # The OLD file is still swept, unconditionally, and that is not superstition: the DLL now REFUSES
    # to run with an mh_harness.ini beside the exe, so a lane folder that predates this change (or a
    # polygon copied from one) would terminate at boot. Sweeping it here turns a stale tree into a
    # normal run instead of a confusing one.
    harn = os.path.join(host_dir, "mh_harness.ini")
    if os.path.isfile(harn):
        os.remove(harn)
    # A scenario driven by a launch verb needs its save INSIDE the lane -- `--tactical <name>` and
    # `--load <name>` both resolve save\<name>.sav relative to the exe.
    if DEPLOY_SAVE:
        src = resolve_save(DEPLOY_SAVE, os.path.join(machine.POLYGON, "save"))
        if not os.path.isfile(src):
            raise SystemExit(
                "--deploy-save: no such save: %s (not in tools/uiscripts/saves/ either)" % src
            )
        dst_dir = os.path.join(host_dir, "save")
        os.makedirs(dst_dir, exist_ok=True)
        shutil.copy2(src, os.path.join(dst_dir, DEPLOY_SAVE + ".sav"))
    exe = os.path.join(host_dir, "mh.focus.exe")
    # The verb goes BEFORE --skip-intro because the parser takes the first verb it sees and then
    # keeps scanning only for --skip-intro (seams/launch.cpp); the order is the parser's, not taste.
    game_args = (LAUNCH_ARGS.split() if LAUNCH_ARGS else []) + ["--skip-intro"]
    # -PassThru so we learn the PID. Killing by IMAGE NAME would take down every other lane on this
    # machine, which is exactly what per-test lanes and concurrent runs must not do.
    before = set(glob.glob(os.path.join(host_dir, "logs", "*")))
    with boot_lock(host_dir):
        # fork F4H: refuse a launch that the single-instance guard would kill silently.
        conflict = make_lane.lane_conflict(host_dir)
        if conflict:
            # sys.exit, not a None return: a launch that cannot succeed must fail HERE with the
            # reason, not thirty seconds later as "the peer never started". ui_test.py is one process
            # per peer, so this is the peer's own verdict and nothing else is torn down.
            sys.exit(conflict)
        pid = None
        if DESKTOP:
            hold_desktop_once()
            # --desktop: put the game on its own desktop object. Start-Process cannot do this --
            # lpDesktop lives in STARTUPINFO and neither PowerShell nor subprocess exposes it -- so
            # this path is a raw CreateProcessW (tools/desktop.py). The window then cannot reach the
            # interactive desktop whatever creates or shows it, which is the point: three separate
            # hypotheses about WHO shows it have already been wrong.
            pid = desktop.spawn(exe, " ".join(game_args), cwd=host_dir, desktop=DESKTOP)
        else:
            r = mp_run.ps(
                "(Start-Process -FilePath '%s' -ArgumentList %s -WorkingDirectory '%s' "
                "-PassThru).Id" % (exe, ",".join("'%s'" % a for a in game_args), host_dir)
            )
            for tok in (getattr(r, "stdout", "") or "").split():
                if tok.strip().isdigit():
                    pid = int(tok.strip())
        if pid:
            _LOCAL_PIDS.append(pid)
            retain_exit_handle(pid)
            # tooling:TL-SUITE-TEARDOWN -- the SAME kill-on-close job TL-RIG6 gave the shim, now on
            # the game peer itself: a runner killed by --jobs' per-test timeout (or anything else
            # that tears down ui_test.py without running peer_kill) no longer orphans mh.focus.exe
            # to pin the lane's mh.dll for the next row that shares it (share_lanes). Never fatal if
            # it fails (no permission, etc.) -- the peer just runs unprotected by this mechanism,
            # same as before it existed.
            job = win_job.assign_kill_on_close(pid)
            if job:
                _LOCAL_JOBS.append(job)
            _record_launch_pid(
                host_dir, pid
            )  # TL-RIG7 -- for the NEXT local_launch() into this lane
        wait_past_pack_load(host_dir, before, pid=pid)
    return pid


# ---- the machine-wide BOOT lock ------------------------------------------------------------------
#
# Lanes SHARE their resource packs -- `mh.rsr` and friends are symlinks to one physical file, which is
# what makes a lane 3.5 MB instead of 300. Two instances loading packs at the same moment collide on
# that file, and the loser does not fail loudly: `rsr::TryReadRsrFile` falls through to the same modal
# a missing disc raises, so the symptom is the blocking
#
#     Insert 'Mission: Humanity' CD into your CD-ROM drive
#
# on a machine with no CD-ROM drive at all (the disc-check RE: that dialog has TWO sources, and the
# packs-not-readable one is the non-obvious half). Measured 2026-07-28: of four lanes launched in the
# same second, three came up with the modal and one booted; a modal also blocks the frame loop, so the
# offscreen keeper stops running and the window becomes visible again -- the "modals about cd" the
# user saw.
#
# The fix is to serialise only the PACK-LOAD window, not the run: one launcher at a time until the
# game has presented its first frame. Boot is a few seconds, so a 12-test suite pays a few seconds per
# test, and the tests still overlap for the other 99% of their life. Cross-PROCESS (each peer is
# launched by its own ui_test.py), so it is a lock FILE, not a threading.Lock.
# The class MOVED to make_lane.py (2026-09-10): make_lane now guards its own provisioning with the
# same lock, and it cannot import ui_test without a cycle. Re-exported here so every existing
# `ui_test.boot_lock` caller (this file, test_ui.py) is unchanged.
BOOT_LOCK = make_lane.BOOT_LOCK
BOOT_LOCK_STALE = make_lane.BOOT_LOCK_STALE
boot_lock = make_lane.boot_lock


def _alive(pid):
    """Is this pid still running? None/unknown -> True, so the caller waits rather than guesses."""
    if not pid or os.name != "nt":
        return True
    try:
        import ctypes
        from ctypes import wintypes

        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.OpenProcess.restype = wintypes.HANDLE
        k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        k32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        h = k32.OpenProcess(0x1000, False, int(pid))  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return False
        code = wintypes.DWORD()
        ok = k32.GetExitCodeProcess(h, ctypes.byref(code))
        k32.CloseHandle(h)
        return bool(ok) and code.value == 259  # STILL_ACTIVE
    except Exception:
        return True


def wait_past_pack_load(host_dir, before, timeout=60, pid=None):
    """Block until this peer has presented a frame (or `timeout`), holding the boot lock.

    The first frametime row is the honest 'done loading' signal: the present hook only fires once the
    game is running frames, which is downstream of every pack read. Waiting on a fixed sleep instead
    would be a guess that gets stale the moment the machine changes.

    LIVENESS IS CHECKED, AND THAT IS A STARVATION FIX, NOT A TIDY-UP (fork F4H). This lock is
    MACHINE-WIDE: everything else on the box that wants to boot a game is queued behind it. A peer
    that dies during boot -- a lane-number collision, a crash, a refused module -- can never present,
    so the wait ran to its full 60 s every time and the lock was held for 50-odd seconds after the
    process it was protecting had ceased to exist. Measured in the F4H before-run: the dead peer held
    it 60 s and the three innocent lanes behind it waited 45 s, 60 s and 66 s, two of them
    NEAR-MISSing their budgets. Noticing the corpse turns that into ~1 s.

    `pid` is optional because one caller cannot always learn it; without it the behaviour is exactly
    what it was, which is the right default for a guard that must never be the reason a run fails.
    """
    deadline = time.time() + timeout
    run = None
    while time.time() < deadline:
        if run is None:
            new = [
                d
                for d in set(glob.glob(os.path.join(host_dir, "logs", "*"))) - before
                if os.path.isdir(d)
            ]
            # SES1: the PROCESS directory -- mh_frametime.log's boot rows are written there, long
            # before any lobby could create a session directory to outrank it.
            menu = [d for d in new if "_menu_" in os.path.basename(d)]
            if menu or new:
                run = max(menu or new, key=os.path.getmtime)
        if run:
            ft = os.path.join(run, "mh_frametime.log")
            try:
                if os.path.getsize(ft) > 0:
                    return True
            except OSError:
                pass
        if not _alive(pid):
            # Say it is DEAD, not that it was slow: the two need different remedies and the old
            # message only ever suggested the wrong one (a bigger budget).
            print(
                "  [boot] %s EXITED during boot without presenting a frame after %.0fs -- "
                "releasing the boot lock now instead of waiting out the %ds budget. Read the lane's "
                "mh_net.log for the `; EXIT` witness line; a clean exit here is usually a lane-number "
                "collision (tools/lane_alloc.py) or a refused module."
                % (os.path.basename(host_dir), timeout - (deadline - time.time()), timeout)
            )
            return False
        time.sleep(0.5)
    print(
        "  [boot] %s did not present a frame within %ds -- releasing the boot lock anyway"
        % (os.path.basename(host_dir), timeout)
    )
    return False


def local_new_run(host_dir, before, deadline):
    """The PROCESS ("menu") directory this launch created.

    SES1: a peer that reaches a lobby creates further directories, one per match, and they are
    newer. This function's answer is what the runner then polls for mh_uidrive.log and drops
    rig_*.flag into for the whole scenario -- both of which the DLL deliberately keeps in the process
    directory -- so a session directory winning the mtime sort here would strand the runner on an
    empty log and every multi-peer scenario would time out rather than run."""
    while time.time() < deadline:
        now = set(glob.glob(os.path.join(host_dir, "logs", "*")))
        new = [d for d in now - before if os.path.isdir(d)]
        menu = [d for d in new if "_menu_" in os.path.basename(d)]
        if menu or new:
            return max(menu or new, key=os.path.getmtime)
        time.sleep(1)
    return None


def local_script_status(run_dir):
    log = os.path.join(run_dir, "mh_uidrive.log")
    if not os.path.isfile(log):
        return None
    with open(log, errors="replace") as f:
        txt = f.read()
    if "; [script] COMPLETE" in txt:
        return "COMPLETE"
    if "TIMEOUT at step" in txt:
        return "TIMEOUT"
    # A click that resolved no widget ends the script THERE (D15). This has to be a TERMINAL status
    # like the two above, or the runner reads a finished peer as "still walking" and waits out the
    # whole --timeout -- which is the exact cost the fail-fast was added to remove.
    if "; [script] ABORT at step" in txt:
        return "ABORT"
    return None


_LOCAL_PIDS = []
_LOCAL_JOBS = []  # win_job handles, TL-SUITE-TEARDOWN -- kept alive so kill-on-close stays armed

# ---- D15: what a dead peer's EXIT CODE says -----------------------------------------------------
# A pid tells you the process is gone. The exit code tells you WHICH WAY it went, and that is the
# whole question cause #2 has been stuck on: four hosts stopped at once with no WER report, no
# Application-log event and nothing in any game log, and "died" covered a clean ExitProcess, an
# abort, a fault WER happened not to record, and this runner's own kill equally well.
#
# The number only survives if somebody is holding a HANDLE. Once the last one closes, Windows frees
# the process object and the pid means nothing (worse: it can be reused, which is how a liveness
# probe reads a dead peer as alive). So a handle is opened at launch and deliberately never closed --
# one kernel handle per lane for the life of a suite run, against a diagnosis that has cost two
# sessions so far.
_EXIT_HANDLE = {}  # pid -> HANDLE, held for the process's whole lifetime and past it


def retain_exit_handle(pid):
    """Hold a handle on `pid` so GetExitCodeProcess still works after it dies."""
    try:
        import ctypes

        SYNCHRONIZE, PROCESS_QUERY_LIMITED_INFORMATION = 0x00100000, 0x1000
        h = ctypes.windll.kernel32.OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid)
        )
        if h:
            _EXIT_HANDLE[int(pid)] = h
    except Exception:  # a diagnostic must never be the thing that fails a run
        pass


def peer_exit_code(pid):
    """The exit code of a peer we launched, or None if we cannot tell."""
    h = _EXIT_HANDLE.get(int(pid)) if pid else None
    if not h:
        return None
    try:
        import ctypes

        code = ctypes.c_ulong(0)
        if not ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code)):
            return None
        return int(code.value)
    except Exception:
        return None


def describe_exit(code):
    """Name the exit code, in the vocabulary of the things that could have produced it.

    STILL_ACTIVE (259) is listed because it is genuinely ambiguous -- Windows returns it for a live
    process AND for one that exited with 259 -- and an ambiguity stated is worth more than a wrong
    confident label, which is the failure mode this whole item exists to fix.
    """
    if code is None:
        return "exit code unavailable (no handle was retained for this peer)"
    known = {
        0: "0 -- a CLEAN exit: ExitProcess(0) or _exit(0), i.e. the GAME chose to stop. Check "
        "mh_net.log for the 'EXIT' witness line, which names the path and the caller",
        1: "1 -- TerminateProcess(.., 1): killed from OUTSIDE by this rig (local_kill / proc.kill), "
        "so look at what the runner did, not at the game",
        3: "3 -- abort()",
        259: "259 -- STILL_ACTIVE, which is ambiguous: either the process is running after all, or "
        "it really exited with 259. Do not read it as dead",
    }
    if code in known:
        return known[code]
    if code >= 0xC0000000:
        nt = {
            0xC0000005: "ACCESS_VIOLATION",
            0xC0000017: "NO_MEMORY",
            0xC000001D: "ILLEGAL_INSTRUCTION",
            0xC0000025: "NONCONTINUABLE_EXCEPTION",
            0xC0000026: "INVALID_DISPOSITION",
            0xC000008C: "ARRAY_BOUNDS_EXCEEDED",
            0xC0000090: "FLOAT_INVALID_OPERATION",
            0xC0000094: "INTEGER_DIVIDE_BY_ZERO",
            0xC00000FD: "STACK_OVERFLOW",
            0xC0000135: "DLL_NOT_FOUND",
            0xC0000142: "DLL_INIT_FAILED",
            0xC000013A: "CONTROL_C_EXIT",
            0xC0000409: "STACK_BUFFER_OVERRUN / __fastfail",
            0xC0000374: "HEAP_CORRUPTION",
        }.get(code)
        # NOT "proof a fault killed this process" -- that claim outran its evidence and cost fork
        # F5H three gate reds read as access violations (dead-ends G187). ExitProcess/_exit set the
        # exit code to WHATEVER they are handed, and this binary's own utils_abort ends in
        # _exit(status) (mh_addrs.gen.h: "_exit raises no exception, so the process vanishes with
        # no WER report"). So an NTSTATUS-shaped code is consistent with a fault AND with the game
        # choosing to die; the code alone cannot separate them. Two things can, and both are cheap:
        #   * a `; EXIT ...` line in the lane's mh_net.log -> self-driven, and it names the caller
        #     (exit_witness() reads it, arm-banner checked)
        #   * a WER record / minidump -> a real unhandled exception -- but a peer on the isolated
        #     mh_rig desktop (the DEFAULT) is INVISIBLE to WER (G186), so "no dump" is not "no fault".
        return (
            "0x%08X -- an NTSTATUS-shaped exit code%s. Not by itself proof of a fault: "
            "utils_abort ends in _exit(status) and would set this same code raising nothing. "
            "Check the lane's mh_net.log for `; EXIT` (self-driven, names the caller); a WER "
            "record means a real fault, but its ABSENCE proves nothing under --desktop (G186)."
            % (code, (" (%s, IF it was a fault)" % nt) if nt else "")
        )
    return "%d (0x%08X) -- not a code this rig produces; the game exited on its own with it" % (
        code,
        code,
    )


def local_kill(pid=None):
    """Kill ONE local peer by pid, or every peer this process started -- and DO NOT RETURN until
    each one is actually gone (tooling:TL-SUITE-TEARDOWN).

    Deliberately NOT `Get-Process mh.focus | Stop-Process`: with per-test lanes or concurrent
    determinism runs there are several instances on this machine, and killing by image name would
    take down somebody else's run -- a cross-test failure that would look like a flaky test.

    `Stop-Process -Force` returns as soon as PowerShell has ISSUED the kill, not once Windows has
    actually torn the process down -- this is the same race TL-RIG7's _wait_for_prior_launch_exit
    already had to work around for a REDEPLOY, generalised here for the caller that must not report
    a verdict (or hand the lane back to share_lanes) while a peer might still be exiting. Waits a
    bounded few seconds, then falls back to a direct TerminateProcess by pid (win_job's own
    primitive) for anything Stop-Process missed.
    """
    pids = [pid] if pid else list(_LOCAL_PIDS)
    if not pids:
        return
    mp_run.ps(
        "foreach ($p in %s) { Stop-Process -Id $p -Force -ErrorAction SilentlyContinue }"
        % ",".join(str(p) for p in pids)
    )
    for p in pids:
        if not win_job.wait_gone(p, timeout=5):
            win_job.process_terminate(p)
            win_job.wait_gone(p, timeout=5)
        if p in _LOCAL_PIDS:
            _LOCAL_PIDS.remove(p)


# ---- remote (VM) client peer ----------------------------------------------------------------------


class _EmptyResult:
    stdout = ""
    stderr = ""
    returncode = 1
    # NOT AN ANSWER (fork F4H). `returncode = 1` is what a poll loop needs -- "no result yet" -- and
    # it is exactly wrong for a caller that ACTS on a failure, because a timed-out ssh and a command
    # that ran and failed become the same object. That is how a busy VM produced
    #   [.38] ABORT: schtasks /create failed -- is anyone LOGGED ON at that VM's console? ()
    # in a gate run: nobody had logged out, the create never ran, and the empty parenthesis at the end
    # is the whole evidence the message had. Callers that abort must read this flag first.
    timed_out = True


# Per-probe ssh timeout for the readiness poll. Short ON PURPOSE: the caller polls on a fixed budget,
# so a hung poll must cost one poll, not a third of the budget (see remote_listening).
PROBE_SSH_TIMEOUT = 8


def remote(args, ip, cmd, timeout=25, tries=1):
    # A transient ssh hang (busy VM) must NOT abort a whole run -- poll loops just see "no result yet".
    #
    # `tries` > 1 is for the ONE-SHOT calls, not the polls (fork F4H). A poll loop deliberately keeps
    # this cheap: a hung probe must cost one poll, not a third of the budget, and the next poll is the
    # retry. A one-shot -- deploy, schtasks create/run -- has no next poll, so a timeout there becomes
    # a verdict about the VM, and the verdict was wrong twice in one session's gate runs. Retrying
    # here is not a wider timeout: the budget per attempt is unchanged, and what is added is a second
    # ATTEMPT at a command that never ran.
    for attempt in range(max(1, tries)):
        try:
            return mp_run.ssh(args.ssh_key, args.vm_user, ip, cmd, timeout=timeout)
        except subprocess.TimeoutExpired:
            if attempt + 1 < max(1, tries):
                print("    (ssh to %s timed out -- retrying, attempt %d)" % (ip, attempt + 2))
                continue
            print("    (ssh to %s timed out -- retrying next poll)" % ip)
    return _EmptyResult()


def deploy_peer_exe(args, ip, d, fwd):
    """Make the peer's mh.focus.exe match the mode this run wants. Returns False to abort the launch.

    Two shapes, one file name. Normal: the import-patched mh.focus.exe from the local install.
    --stock-exe: retail mh.exe bytes under that same name, plus the msvfw32 shim next to it.

    ABORTING on failure is deliberate, and matters MORE here than for mh.dll. If the shim fails to
    land, a stock-exe peer does not fail loudly -- it boots as a clean retail game with no mh.dll at
    all, and the scenario then fails somewhere far away for a reason that looks nothing like "the
    DLL never loaded". A wrong-build result is worse than no result (see the mh.dll copy below).
    """
    want = "stock" if getattr(args, "stock_exe", False) else "focus"
    # tries=3 (fork F4H): a timed-out read of the marker is NOT "the marker says something else", but
    # it used to be -- so a busy VM re-staged a 1.9 MB exe over the link that had just hung, right in
    # front of the launch whose rendezvous window the delay then ate.
    r = remote(args, ip, "type %s%s%s 2>nul" % (d, "\\", PEER_EXE_MARKER), tries=3)
    if getattr(r, "timed_out", False):
        print(
            "    [%s] ABORT: ssh timed out three times reading the exe marker -- the VM is "
            "unreachable or saturated. Re-deploying on a non-answer is how this used to hide." % ip
        )
        return False
    have = (getattr(r, "stdout", "") or "").strip()

    if have != want:
        src = os.path.join(machine.POLYGON, "mh.exe" if want == "stock" else "mh.focus.exe")
        if not os.path.isfile(src):
            print("    [%s] ABORT: no %s to deploy" % (ip, src))
            return False
        if (
            mp_run.scp(
                args.ssh_key, src, "%s@%s:%s/mh.focus.exe" % (args.vm_user, ip, fwd)
            ).returncode
            != 0
        ):
            print("    [%s] ABORT: could not deploy the %s exe" % (ip, want))
            return False
        print("    [%s] staged the %s exe" % (ip, want))

    if want == "stock":
        # Pushed EVERY run, not just on a mode change: it is a build output like mh.dll, and a peer
        # holding a stale shim is exactly the silent-inert failure above.
        if (
            mp_run.scp(
                args.ssh_key, SHIM, "%s@%s:%s/msvfw32.dll" % (args.vm_user, ip, fwd)
            ).returncode
            != 0
        ):
            print("    [%s] ABORT: could not deploy the msvfw32 shim" % ip)
            return False
    elif have != want:
        # Leaving a shim next to an import-patched exe would load mh.dll TWICE over.
        remote(args, ip, "del /q %s%smsvfw32.dll 2>nul" % (d, "\\"))

    if have != want:
        remote(args, ip, "echo %s> %s%s%s" % (want, d, "\\", PEER_EXE_MARKER))
    return True


def remote_launch(
    args, ip, script_src, script_name, timeout_frames, harness_steps=0, is_host=False
):
    d = args.vm_dir
    fwd = d.replace("\\", "/")
    # tooling:TL-SUITE-VMSESSION, and FIRST -- before anything else touches this VM. A session that
    # is Disc (or Active on something other than the console) never presents a frame no matter what
    # gets deployed to it, so every other VM unit that reds through this function ("host never
    # opened 6501") was really this. Refuse (or reattach) here, once, rather than let each caller
    # discover it 90s later as a listening-port timeout that names the wrong layer.
    if not mp_run.ensure_vm_console_session(args.ssh_key, args.vm_user, ip):
        print(
            "    [%s] ABORT: VM SESSION REFUSED -- not Active on its console; refusing to launch "
            "rather than waiting out a 90s 'never opened' timeout" % ip
        )
        return False
    # TL-RIGKILL (2026-09-18): kill any leftover peer process BEFORE the first upload, not just at
    # teardown. An aborted/killed prior run (this process crashed, was Ctrl+C'd, or the whole rig
    # tool was killed externally) skips peer_kill()'s normal end-of-run remote_kill(), so the VM keeps
    # running mh.focus.exe -- and Windows holds an open exe/DLL file for write, so the NEXT run's
    # `deploy_peer_exe` scp of mh.focus.exe or the msvfw32 shim fails ("scp: dest open ... Failure" ->
    # "ABORT: could not deploy the msvfw32 shim"), poisoning a run that never even reached this peer's
    # own game code. Doing it here, first, means a leftover process can never win that race -- it is
    # always dead before the first byte of this run's deploy goes over the wire.
    print("    [%s] killing any leftover peer process before deploy" % ip)
    remote_kill(args, ip)
    # A failed DLL copy used to print "scp FAILED" and carry on -- so the peer ran whatever mh.dll it
    # happened to have, and the run reported on the WRONG build. Seen 2026-07-26 (a "Broken pipe" left
    # the host on a stale DLL while the client had the new one). A stale-build result is worse than no
    # result, so this is fatal: retry once, then refuse to launch.
    #
    # mp:T3g -- this scp'd the bare module-level `DLL` constant, not `g_dll()`, so `--dll <path>` had
    # NO EFFECT on a VM-topology run (local_launch already calls g_dll(); this remote twin did not).
    # The "[dll] deploying ... (overrides the Release build)" banner prints and the deploy "succeeds"
    # regardless, so a wrong-build run looks identical to a correct one from this function's own
    # output. The peer's own `mh_net.log` SESSION_BEGIN `build=` field is the only thing that actually
    # proves which binary ran -- read that, not this banner, before trusting a `--dll`-overridden run
    # (dead-ends G316).
    if not deploy_peer_exe(args, ip, d, fwd):
        return False
    for attempt in (1, 2):
        if (
            mp_run.scp(
                args.ssh_key, g_dll(), "%s@%s:%s/mh.dll" % (args.vm_user, ip, fwd)
            ).returncode
            == 0
        ):
            break
        if attempt == 2:
            print("    [%s] ABORT: could not deploy mh.dll -- refusing to test a stale build" % ip)
            return False
        print("    [%s] mh.dll copy failed -- retrying once" % ip)
        time.sleep(2)
    # THE SATELLITES (fork F4B), on the same fatal terms as mh.dll and for a sharper reason. A VM has
    # no lane folder: its game directory persists between runs, so an mh_net.dll that failed to copy
    # leaves the PREVIOUS build's transport talking to this build's mh.dll -- a mismatched pair, and
    # the ABI check would refuse it at boot, turning a failed scp into "multiplayer stopped working".
    # A peer that never had one at all would run the no-module configuration and fail every MP
    # scenario for a reason that looks nothing like a missing file.
    #
    # UNCONDITIONAL HERE, unlike the local refresh: a VM directory is not provisioned per test, so
    # there is no absent-arm lane to protect. The absent arm is a LOCAL lane (module_absent), which
    # is also why --local is the suite's default.
    omit = set()
    for spec in getattr(args, "omit_satellite", None) or []:
        role, _, sat = spec.rpartition(":")
        if role == "" or (role == "host") == bool(is_host):
            omit.add(sat)
    for name in SATELLITES:
        if name in omit:
            # mp:D29 (D3): the absent arm, on a VM. Delete, then PROVE it is gone -- a peer that kept
            # a stale copy would run configuration (2) under a configuration (1) name, and its
            # verdict would describe the wrong build (the failure det_run_report's `configs`
            # check exists to catch one step later).
            remote(args, ip, "del /q %s\\%s 2>nul" % (d, name), tries=3)
            r = remote(args, ip, 'if exist "%s\\%s" (exit 1) else (exit 0)' % (d, name), tries=3)
            if r.returncode != 0:
                print(
                    "    [%s] ABORT: --omit-satellite %s but the peer still has it (or the check "
                    "could not run) -- refusing to run a configuration the run does not name"
                    % (ip, name)
                )
                return False
            print("    [%s] %s OMITTED: not deployed, and absent on the peer" % (ip, name))
            continue
        src = os.path.join(os.path.dirname(g_dll()), name)
        if not os.path.isfile(src):
            print("    [%s] ABORT: no %s to deploy (build the mh.sln first)" % (ip, name))
            return False
        for attempt in (1, 2):
            if (
                mp_run.scp(
                    args.ssh_key, src, "%s@%s:%s/%s" % (args.vm_user, ip, fwd, name)
                ).returncode
                == 0
            ):
                break
            if attempt == 2:
                print(
                    "    [%s] ABORT: could not deploy %s -- refusing to test a stale pair"
                    % (ip, name)
                )
                return False
            print("    [%s] %s copy failed -- retrying once" % (ip, name))
            time.sleep(2)
    mp_run.scp(args.ssh_key, script_src, "%s@%s:%s/%s" % (args.vm_user, ip, fwd, script_name))
    ini_local = os.path.join(_scratch(), "ui_test_vm_%s.ini" % ip.replace(".", "_"))
    with open(ini_local, "w", newline="\r\n") as f:
        _ini_text = make_ini(script_name, timeout_frames, harness_steps, is_host)
        f.write(_ini_text)
    mp_run.scp(args.ssh_key, ini_local, "%s@%s:%s/mh_net.ini" % (args.vm_user, ip, fwd))
    # effective_config.json (tooling:TL-SUITE-INIMERGE): staged at the VM's root here, next to
    # mh_net.ini, because this is the one place that already has the exact text just deployed.
    # peer_launch copies it into the actual run's log directory once that directory exists (it does
    # not yet -- the game has not even been scheduled below).
    cfg_local = os.path.join(
        _scratch(), "ui_test_vm_%s_effective_config.json" % ip.replace(".", "_")
    )
    write_effective_config(cfg_local, _ini_text, dll_dir=os.path.dirname(g_dll()))
    if os.path.isfile(cfg_local):
        mp_run.scp(
            args.ssh_key, cfg_local, "%s@%s:%s/effective_config.json" % (args.vm_user, ip, fwd)
        )
    key_local = write_rig_key(_scratch())  # same PSK as the host peer, or the join is refused
    mp_run.scp(args.ssh_key, key_local, "%s@%s:%s/mh_key.txt" % (args.vm_user, ip, fwd))
    # ONE FILE (fork F2G): the [harness] block rode in the mh_net.ini above, so there is no second
    # scp. The old file is deleted UNCONDITIONALLY now -- it used to be deleted only on a non-harness
    # run, to stop a stale stop_step halting a plain UI walk. The DLL refuses to run with one beside
    # the exe at all, so on a VM that has been part of any earlier determinism run this delete is
    # what keeps the peer bootable rather than merely un-perturbed.
    remote(args, ip, "del /q %s\\mh_harness.ini 2>nul" % d)
    bat_local = os.path.join(_scratch(), "ui_test_run.bat")
    with open(bat_local, "w", newline="\r\n") as f:
        f.write("@echo off\r\ncd /d %s\r\nmh.focus.exe --skip-intro\r\n" % d)
    mp_run.scp(args.ssh_key, bat_local, "%s@%s:%s/ui_test_run.bat" % (args.vm_user, ip, fwd))
    remote(args, ip, "schtasks /delete /tn uitest /f 2>nul")
    # CHECK THESE TWO. They used to be fire-and-forget with an unconditional `return True`, and the
    # commonest rig failure by far goes straight through that hole: `/ru <user> /it` needs an INTERACTIVE
    # TOKEN, so if nobody is logged on at the VM's console the create fails outright -- and the caller
    # then saw only peer_launch()'s silent None -> "return 1" with NO message at all, because every
    # abort path in this function prints but this one could not fail. Cost a full diagnosis pass on
    # 2026-08-29 (both rig VMs were logged out; `schtasks /query /tn uitest` on the VM answered "The
    # system cannot find the file specified", i.e. the task had never been created). Name the cause here
    # instead: the peer is either not logged on or the task could not be scheduled.
    # THREE TRIES, AND A TIMEOUT IS REPORTED AS A TIMEOUT (fork F4H). These two are one-shots on a VM
    # that the gate has just made busy, and a timed-out ssh used to arrive here as returncode 1 -- so
    # the run aborted with the logged-out diagnosis above, which was false, and with an empty evidence
    # string, which is how you can tell. Measured: this is what redded the gate's `det` unit twice
    # (client never launched, "need >=2 peers with logs; got 1"), while the identical command passes
    # on an idle box.
    r = remote(
        args,
        ip,
        'schtasks /create /tn uitest /tr "%s\\ui_test_run.bat" /sc once /st 00:00 /ru %s /it /f'
        % (d, args.vm_user),
        tries=3,
    )
    if getattr(r, "timed_out", False):
        print(
            "    [%s] ABORT: ssh timed out three times on `schtasks /create` -- the command never "
            "ran, so this says nothing about the task or the console session. The VM is unreachable "
            "or saturated." % ip
        )
        return False
    if getattr(r, "returncode", 0) != 0:
        print(
            "    [%s] ABORT: schtasks /create failed -- is anyone LOGGED ON at that VM's console? "
            "/it needs an interactive token, so a logged-out peer cannot be launched. (%s)"
            % (ip, (getattr(r, "stderr", "") or getattr(r, "stdout", "") or "").strip()[:160])
        )
        return False
    r = remote(args, ip, "schtasks /run /tn uitest", tries=3)
    if getattr(r, "timed_out", False):
        print(
            "    [%s] ABORT: ssh timed out three times on `schtasks /run` -- the task exists and may "
            "or may not have been started; nothing here is evidence either way." % ip
        )
        return False
    if getattr(r, "returncode", 0) != 0:
        print(
            "    [%s] ABORT: schtasks /run failed -- the task exists but would not start. (%s)"
            % (ip, (getattr(r, "stderr", "") or getattr(r, "stdout", "") or "").strip()[:160])
        )
        return False
    return True


def remote_newest_run(args, ip):
    """The VM peer's newest PROCESS ("menu") directory -- see local_new_run for why not the newest.

    Lists ALL of them rather than taking the first line, because `/o-d` newest-first would hand back
    a session directory the moment that peer reached a lobby, and this name is what the runner then
    polls (mh_uidrive.log) and drops rig_*.flag into for the rest of the scenario."""
    r = remote(args, ip, "dir /b /ad /o-d %s\\logs 2>nul" % args.vm_dir)
    names = [ln.strip() for ln in (r.stdout or "").splitlines() if ln.strip()]
    if not names:
        return None
    menu = [n for n in names if "_menu_" in n]
    return (menu or names)[0]


def remote_script_status(args, ip, run):
    r = remote(args, ip, "type %s\\logs\\%s\\mh_uidrive.log 2>nul" % (args.vm_dir, run))
    txt = r.stdout or ""
    if "; [script] COMPLETE" in txt:
        return "COMPLETE"
    if "TIMEOUT at step" in txt:
        return "TIMEOUT"
    if "; [script] ABORT at step" in txt:  # D15, same terminal status as the local path
        return "ABORT"
    return None


# ---- peer rendezvous: ferry `signal <name>` from one peer to the others ---------------------------
#
# A multi-peer script sometimes has to wait on a fact only the OTHER peer can observe. The case this was
# built for: the host must not click Start until the CLIENT's UI has reached its lobby, and no protocol
# message says that -- the host's earliest observable is the JOIN admit, which lands about a second too
# early, so the client got ~2 frames of lobby and could never settle or capture (measured 2026-07-28).
#
# So the runner ferries it. A peer's script runs `signal lobby`, which only writes a marker to its own
# log; we notice the marker here and drop `rig_lobby.flag` into every OTHER peer's run dir, where their
# `awaitsignal lobby` sees it. Deliberately a FILE the rig writes, not a new wire message: inventing game
# protocol to satisfy a test would make the test's subject different from the shipped game.
SIGNAL_RE = re.compile(r"; \[script\] SIGNAL (\S+)")


def peer_signals(args, ip, run):
    """Signal names this peer has emitted so far."""
    if ip is None:
        log = os.path.join(run, "mh_uidrive.log")
        txt = open(log, errors="replace").read() if os.path.isfile(log) else ""
    else:
        txt = (
            remote(args, ip, "type %s\\logs\\%s\\mh_uidrive.log 2>nul" % (args.vm_dir, run)).stdout
            or ""
        )
    return set(SIGNAL_RE.findall(txt))


def deliver_signal(args, peer, name):
    """Drop rig_<name>.flag into `peer`'s run dir."""
    if not peer.get("run"):
        return
    if peer["ip"] is None:
        try:
            open(os.path.join(peer["run"], "rig_%s.flag" % name), "w").close()
        except OSError as e:
            print("  [rig] could not deliver signal %r to %s: %s" % (name, peer["key"], e))
        return
    local_flag = os.path.join(_scratch(), "rig_%s.flag" % name)
    open(local_flag, "w").close()
    mp_run.scp(
        args.ssh_key,
        local_flag,
        "%s@%s:%s/logs/%s/rig_%s.flag"
        % (args.vm_user, peer["ip"], args.vm_dir.replace("\\", "/"), peer["run"], name),
    )


# mp:R4b -- `--signal-touch NAME=PATH`: a script signal that reaches OUTSIDE the pair. The ferry
# above carries a signal to the OTHER PEERS; this carries it to the RUNNER'S CALLER (test_ui.py),
# which owns things a script cannot reach -- the relay process a relayed scenario runs against, in
# the first instance. A file, for the ferry's own reason: the caller already polls files, and a
# touch is the one gesture that needs no protocol. Touched once per name per run.
SIGNAL_TOUCH = {}  # name -> path, from --signal-touch
_signal_touched = set()


def touch_for_signal(name):
    path = SIGNAL_TOUCH.get(name)
    if path is None or name in _signal_touched:
        return
    _signal_touched.add(name)
    try:
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        open(path, "w").close()
        print("[rig] signal %r -> touched %s" % (name, path))
    except OSError as e:
        print("  [rig] could not touch %s for signal %r: %s" % (path, name, e))


def pump_signals(args, peers, delivered):
    """One poll: ferry every new signal to the peers that did not emit it."""
    for src in peers:
        if not src.get("run"):
            continue
        for name in peer_signals(args, src["ip"], src["run"]):
            touch_for_signal(name)
            for dst in peers:
                if dst is src:
                    continue
                key = (dst["key"], name)
                if key in delivered:
                    continue
                delivered.add(key)
                print("[rig] signal %r from %s -> %s" % (name, src["key"], dst["key"]))
                deliver_signal(args, dst, name)


def remote_kill(args, ip):
    remote(args, ip, "taskkill /im mh.focus.exe /f 2>nul & schtasks /delete /tn uitest /f 2>nul")


def remote_listening(args, ip, port, transport="tcp"):
    """Is the host's lobby port open yet?

    Uses `netstat` over a SHORT-timeout ssh, deliberately, and the reason is a gate outage worth
    remembering (2026-07-27). This probe used to launch PowerShell
    (`Get-NetTCPConnection -State Listen`). PowerShell's cold start on a VM that is already running
    the game routinely exceeds `remote()`'s 25 s ssh timeout -- and the readiness loop that calls this
    has a HARD 90 s budget, so three or four hung polls exhaust it and the run aborts with
    "host never became LISTENING". Measured while that was happening: the game had been listening on
    6501 since t+17.5 s. Every multi-peer test and the whole determinism gate failed for ~an hour on a
    host that was up the entire time -- an orchestration failure that reads exactly like a functional
    regression, because the message names the game's state and not the probe's.

    Two changes, both about the probe never being the thing that fails: `netstat` via cmd costs a
    fraction of a PowerShell start, and a short per-probe timeout means a hung poll costs one poll
    instead of a third of the budget.

    NOT a TCP connect from here, though that is cheaper still and needs no ssh: the port belongs to
    the game's authenticated transport, so probing it would register as a peer connecting and
    immediately dropping -- the test would perturb what it measures.

    mp:T1 made this transport-aware. UDP has no LISTEN state at all -- a bound datagram socket is
    simply a row in `netstat -an -p UDP` -- so the tcp form above matches nothing for a udp host and
    the readiness loop burns its whole budget on a host that came up on time. Measured before this
    branch existed: `[det] host never became LISTENING within 90s -- aborting` against that VM's own
    log reading `net: udp HOST listening on :6501 as player 0 (K=3) [key set]` seven seconds in.
    Two ports of the same number are also two different rows, so the udp probe must not match a
    leftover tcp one: `-p UDP` scopes the table rather than filtering its text.
    """
    cmd = (
        'cmd /c netstat -an -p UDP ^| findstr /c:":%d "' % port
        if transport == "udp"
        else 'cmd /c netstat -an ^| findstr /c:":%d " ^| findstr /i "LISTENING"' % port
    )
    r = remote(args, ip, cmd, timeout=PROBE_SSH_TIMEOUT)
    return bool((r.stdout or "").strip())


def pin_setup(args, ip, ip_val=None, name=None, game=None, pdir=None):
    """PIN a peer's setup.dat (server IP / player name / game name) so a run is deterministic + machine-
    independent (the captured name/game/server text no longer depends on the machine's saved history).
    ip=None -> the LOCAL dev box (args.host_dir); ip='a.b.c.d' -> a VM (pull/push via mp_run scp)."""
    if args.no_pin or not any(v is not None for v in (ip_val, name, game)):
        return
    who = ip or "local"
    fwd = args.vm_dir.replace("\\", "/")
    local_path = os.path.join(pdir or args.host_dir, "setup.dat")
    # tag by LANE for a local peer: two local peers must not share a scratch file
    tag = (who if ip else os.path.basename(pdir or args.host_dir)).replace(".", "_")
    tin = os.path.join(_scratch(), "setup_%s_in.dat" % tag)
    tout = os.path.join(_scratch(), "setup_%s_out.dat" % tag)
    if ip is None:  # pull (local copy / remote scp)
        if not os.path.isfile(local_path):
            print("  [local] WARN: no setup.dat at %s -- skipping pin" % local_path)
            return
        shutil.copy(local_path, tin)
    else:
        mp_run.scp(args.ssh_key, "%s@%s:%s/setup.dat" % (args.vm_user, ip, fwd), tin)
        if not os.path.isfile(tin):
            print("  [%s] WARN: could not pull setup.dat -- skipping pin" % who)
            return
    try:
        setup_dat.set_fields(tin, tout, ip=ip_val, name=name, game=game)
    except Exception as e:
        print("  [%s] WARN: setup.dat pin failed (%s) -- leaving as-is" % (who, e))
        return
    if ip is None:
        shutil.copy(tout, local_path)
    else:
        mp_run.scp(args.ssh_key, tout, "%s@%s:%s/setup.dat" % (args.vm_user, ip, fwd))
    tag = ", ".join(
        "%s=%s" % (k, v) for k, v in (("ip", ip_val), ("name", name), ("game", game)) if v
    )
    print("  [%s] setup.dat pinned (%s)" % (who, tag))


def _solo_lane_dir(name):
    """(Re)provision THIS TREE's own copy of the named solo lane, under a lane NUMBER only this tree
    holds (lane_alloc.tree_slot -- TL-LANECOLLIDE, 2026-09-18), and return its folder.

    WHY THIS EXISTS, separately from the plain `lane=NAME` form below. `workdir/mh_lanes`
    (make_lane.LANE_ROOT) is ONE machine-wide folder and lane_alloc's BLOCKS are ONE machine-wide set
    of numbers -- both built when "a tree" meant the single interactive checkout. A git WORKTREE is a
    second, independent process tree on the same box that imports this exact code and picks the exact
    same folder name AND the exact same number for a plain `lane=NAME` spec, so two worktrees each
    running their own solo scenario collided on both: the shared folder (a provision from one tree
    could stomp the other's files mid-run) and the shared mutex (the DLL's bare, machine-wide
    `MHMutNN`, which is what actually killed the second run -- see lane_alloc.py's header comment for
    the exact symptom). `solo=NAME` sidesteps both: the folder is suffixed with a hash of this tree's
    own absolute repo root (so two trees' "devloop" never alias the same directory), and the lane
    NUMBER comes from lane_alloc's per-tree claim (so two trees' "devloop" never alias the same
    mutex either) -- the existing `lane=NAME` form is left exactly as it was, for callers that already
    coordinate their own numbers by hand.
    """
    slot = lane_alloc.tree_slot()
    if slot is None:
        raise SystemExit(
            "solo=%s: no free per-tree solo lane -- every slot in lane_alloc's %r block is claimed "
            "by another live worktree (`python tools/lane_alloc.py --list` shows who holds which). "
            "Free one with `python tools/lane_alloc.py --release-solo` on the tree that is done with "
            "it, or fall back to an explicit `lane=NAME` with a hand-picked number."
            % (name, lane_alloc.SOLO_BLOCK)
        )
    tag_hash = hashlib.sha1(lane_alloc._tree_tag().encode("utf-8")).hexdigest()[:8]
    d = os.path.join(make_lane.LANE_ROOT, "%s__t%s" % (name, tag_hash))
    ident = make_lane.read_identity(d)
    if ident.get("lane") != slot:
        # First use by this tree, or a stale/foreign folder left at this exact suffixed path -- build
        # (or rebuild) it fresh, under THIS tree's own claimed number. Mirrors make_lane.py main()'s
        # own defaults for a plain `--lane N` call (stock exe, headless, the standard satellite set).
        args = argparse.Namespace(
            src=machine.POLYGON,
            dst=d,
            lane=slot,
            port=0,
            headless=True,
            fps_limit=None,
            extra_ini="",
            dll="",
            stock_exe=True,
            satellite=[],
            omit_satellite=[],
            map_variant=None,  # make_lane._provision reads it (mp:X2a); a solo lane keeps stock Maps
        )
        with make_lane.boot_lock("solo:%s" % name):
            make_lane._provision(args)
    return d


def parse_peer(spec):
    r"""Peer spec -> (ip, script, lane_dir).

        '1.2.3.4:script.txt'    -> a VM               ("1.2.3.4", script, None)
        'lane=ui_h:script.txt'  -> a LOCAL LANE       (None, script, <LANE_ROOT>\ui_h)
        'solo=ui_h:script.txt'  -> a per-TREE LANE    (None, script, <LANE_ROOT>\ui_h__t<hash>),
                                    auto-provisioned under this tree's own lane_alloc.tree_slot()
                                    number -- safe for two worktrees to use the SAME name at once
                                    (TL-LANECOLLIDE); 'lane=' does not get this, deliberately (its
                                    number is whatever the caller provisioned it with by hand).
        'script.txt'            -> the local dev box  (None, script, None)  [--host-dir]

    The lane form is what lets SEVERAL local peers coexist: each needs its own game directory, both
    because mh.dll reads its ini next to the exe and because two peers sharing a folder would fight
    over setup.dat, mh_net.ini and the logs dir.
    """
    if ":" in spec:
        head, tail = spec.split(":", 1)
        # a bare Windows drive path ("F:\...") is NOT ip:script -- disambiguate on the dotted-quad head
        if re.match(r"^\d{1,3}(\.\d{1,3}){3}$", head):
            return head, tail, None
        if head.startswith("lane="):
            name = head[len("lane=") :]
            d = name if os.path.isabs(name) else os.path.join(make_lane.LANE_ROOT, name)
            return None, tail, d
        if head.startswith("solo="):
            return None, tail, _solo_lane_dir(head[len("solo=") :])
    return None, spec, None


# ---- unified peer dispatch: ip=None is the local dev box, ip="a.b.c.d" is a VM (via mp_run ssh/scp) ----

# run token -> the pid that produced it, so a local peer can be killed individually rather than by
# image name (see local_kill).
_RUN_PID = {}
# run token -> (lane directory, launch time). Kept so a peer that DIES can be told apart from a peer
# that HANGS: see peer_liveness() -- the rig called both "STALLED" until 2026-08-01.
_RUN_META = {}
_crash_checked = {}  # run token -> the early liveness probe already fired (it is a PS round trip)
# run token -> (args, ip) for a peer on a VM. peer_liveness takes only the run token, and a VM
# peer's two questions (is it alive, did it fault) both need ssh -- so the transport it needs is
# remembered at launch rather than threaded through every caller.
_RUN_REMOTE = {}


def peer_launch(args, ip, script_src, tf, harness_steps=0, is_host=False, pdir=None):
    """Launch a peer at the menu; return its run token (local dir path / remote dir name), or None.

    `pdir` is the LOCAL peer's game directory. It defaults to --host-dir, but a caller running
    per-test lanes (or two local peers at once) passes a distinct lane folder per peer -- that is what
    keeps one test from inheriting the previous test's setup.dat / ini, and what lets two local peers
    coexist at all."""
    if ip is None:
        d = pdir or args.host_dir
        before = local_existing_runs(d)
        # BEFORE the launch: a crash report is matched by lane + time, and a window that starts after
        # the process did would miss a crash during startup, which is exactly when a bad arm crashes.
        t0 = time.time() - 5
        pid = local_launch(d, script_src, os.path.basename(script_src), tf, harness_steps, is_host)
        run = local_new_run(d, before, time.time() + min(args.timeout, 30))
        if run:
            _RUN_PID[run] = pid
            _RUN_META[run] = (d, t0)
            # effective_config.json (tooling:TL-SUITE-INIMERGE), next to this run's own logs. Read
            # back the ini local_launch just wrote rather than threading its text through the return
            # value -- it is the same tiny file, already flushed to disk. Best-effort: never fails
            # the run (write_effective_config swallows its own write errors; this swallows the read).
            try:
                with open(os.path.join(d, "mh_net.ini"), "r", encoding="utf-8") as fh:
                    write_effective_config(
                        os.path.join(run, "effective_config.json"), fh.read(), dll_dir=d
                    )
            except OSError:
                pass
        return run
    before = remote_newest_run(args, ip)
    # Same reason as the local branch: the crash window must open BEFORE the process does, or a
    # crash during startup falls outside it.
    t0 = time.time() - 5
    if (
        remote_launch(
            args, ip, script_src, os.path.basename(script_src), tf, harness_steps, is_host
        )
        is False
    ):
        return None  # deploy refused (stale build) -- do not pretend this peer ran
    for _ in range(20):  # wait for a NEW run dir (distinct from the previous newest)
        time.sleep(1)
        run = remote_newest_run(args, ip)
        if run and run != before:
            _RUN_META[run] = (args.vm_dir, t0)
            _RUN_REMOTE[run] = (args, ip)
            # The VM twin of the local write above: remote_launch already staged
            # effective_config.json at the VM's game-dir root (it could not know `run` yet); now
            # that the run directory exists, copy it in beside the logs it describes. Best-effort --
            # `remote()` already tolerates/reports its own failures, and a missing artifact here must
            # never fail a scenario the game itself completed.
            remote(
                args,
                ip,
                'copy /y "%s\\effective_config.json" "%s\\logs\\%s\\" >nul 2>nul'
                % (args.vm_dir, args.vm_dir, run),
            )
            return run
    # SAY SO. This used to `return None` mutely and the caller's `if not hrun: return 1` printed
    # nothing either, so a peer that never started looked identical to a crash in the runner -- the
    # whole failure was five log lines that stopped mid-sentence. The deploy and the schtasks calls
    # above are all loud now, so reaching here means the task WAS scheduled and the game still never
    # produced a run directory.
    print(
        "    [%s] ABORT: launched, but no NEW run dir under %s\\logs after 20s (newest is still %r). "
        "The task was scheduled, so suspect the game itself: no console session to draw into, a "
        "crash before the first log write, or a stale mh.focus.exe." % (ip, args.vm_dir, before)
    )
    return None


def peer_status(args, ip, run):
    return local_script_status(run) if ip is None else remote_script_status(args, ip, run)


def peer_ready(args, ip, port):
    t = RUN_TRANSPORT
    return host_listening(port, t) if ip is None else remote_listening(args, ip, port, t)


def _wait_host_ready(args, host_ip, host):
    """Poll until the host is LISTENING (its lobby is up) or its script has already finished.

    The budget is the RUN's own --timeout, not the flat 50 s this used to allow. That 50 s assumed a
    host starts listening shortly after launch, which is not what a MENU-PATH host does: it only
    listens once its walk has created the game. Locally that is ~90-120 s, so every local multi-peer
    test was killed mid-walk -- measured 2026-07-28 with the host at step 10 of 18 and 57 k frames
    rendered, i.e. a healthy run aborted by a budget that never described what it was waiting for.
    Progress is printed while waiting, because a silent two-minute poll is indistinguishable from a
    hang and that is how the too-short budget survived.

    D15: it also gives up the moment the host PROCESS IS GONE. A dead host can never start
    listening, so every second after that is spent proving something already known -- and this loop
    owns the whole --timeout, which is where the suite's worst numbers came from: a host that died
    at 12 s still cost 200 s here (900 s for link_death), and the run then reported a plain
    "never became ready", which reads like a too-short budget and sent two sessions after the
    budgets. It is the SAME confusion peer_verdict was written for on the determinism path; this
    just uses the helper that already existed. Checked every ~10 s rather than every poll because
    peer_liveness costs a PowerShell round-trip.
    """
    deadline, t0, last, last_live = time.time() + args.timeout, time.time(), 0.0, time.time()
    while time.time() < deadline:
        if peer_ready(args, host_ip, args.port) or peer_status(args, host_ip, host["run"]):
            return True
        if host_ip is None and time.time() - last_live >= 10:
            last_live = time.time()
            alive, lines = peer_liveness(host["run"])
            if alive is False:  # None means "cannot tell" -- never read that as dead
                print(
                    "[host] the host PROCESS EXITED after %ds without ever listening on %d/%s -- "
                    "giving up now rather than waiting out the remaining %ds of --timeout."
                    % (
                        int(time.time() - t0),
                        args.port,
                        RUN_TRANSPORT,
                        int(deadline - time.time()),
                    )
                )
                for ln in lines:
                    print(ln)
                # Same evidence discipline as report_dead_peer(): the exit code names a CLASS,
                # not a mechanism -- and "[crash]" prejudged the answer in the label itself (G187).
                print("  [exit] %s" % describe_exit(peer_exit_code(_RUN_PID.get(host["run"]))))
                for wln in exit_witness(host["run"]):
                    print(wln)
                postmortem_archive(host["run"])
                host["dead"] = True  # so the caller does not then blame the budget
                return False
        if time.time() - last >= 20:
            last = time.time()
            print(
                "[host] not listening on %d/%s yet (%ds elapsed, walking to its lobby) ..."
                % (args.port, RUN_TRANSPORT, int(last - t0))
            )
        time.sleep(2)
    return bool(peer_ready(args, host_ip, args.port) or peer_status(args, host_ip, host["run"]))


def peer_kill(args, ip, run=None):
    if ip is None:
        # kill only THIS peer when we know which one it was (per-test lanes / concurrent runs)
        local_kill(_RUN_PID.get(run))
    else:
        remote_kill(args, ip)


# ---- U21: a VM peer's crash evidence lives ON THE VM ----------------------------------------------
# crash_report.py has told a crash from a hang for LOCAL lanes since 2026-08-01, and VM peers were
# never covered -- so a crash on a determinism peer came back as a bare stall, which is the exact
# confusion the local half was built to remove, on the peers that matter most (the determinism gate
# is the main multi-peer user). U21's own access violation was observed on a client VM, so a
# recurrence would still have been reported as "harness STALLED" and nothing more.
#
# Deliberately cmd-only, not PowerShell: PS cold start on a VM already running the game routinely
# exceeds the ssh timeout, and that cost an hour of gate outage once (see remote_listening). Newest
# first, copy a handful, and let the LOCAL parser apply the real `since` filter from each report's
# own EventTime -- the remote ordering only bounds how much we copy.
VM_WER_ROOTS = (
    r"%ProgramData%\Microsoft\Windows\WER\ReportArchive",
    r"%ProgramData%\Microsoft\Windows\WER\ReportQueue",
    r"%LOCALAPPDATA%\Microsoft\Windows\WER\ReportArchive",
    r"%LOCALAPPDATA%\Microsoft\Windows\WER\ReportQueue",
)


def remote_alive(args, ip):
    """Is the game still running on the VM? True/False, or None if the probe itself failed.

    By IMAGE NAME, which would be wrong locally -- several lanes share mh.focus.exe and blaming by
    image name is a cross-test failure this rig refuses elsewhere (local_kill) -- and is right here:
    a VM peer runs exactly one game. None is NOT False: a timed-out ssh must never read as a death.
    """
    r = remote(args, ip, 'cmd /c tasklist /FI "IMAGENAME eq mh.focus.exe" /NH', timeout=20)
    out = (getattr(r, "stdout", "") or "").strip()
    if not out:
        return None
    return "mh.focus.exe" in out.lower()


def remote_crash_lines(args, ip, since, limit=3):
    r"""A VM peer's crash evidence, attributed exactly as a local lane's is.

    TWO SOURCES, and the ORDER MATTERS because the obvious one is the one that cannot work here.
    Measured 2026-08-27 on 192.168.0.38: the rig VMs have
    HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\Disabled = 1, and both WER trees are
    empty -- ReportQueue, ReportArchive, per-machine and per-user. A Report.wer pull aimed at these
    boxes would have been a detector that can never fire, reporting "no crash" for every crash. The
    APPLICATION EVENT LOG is written regardless, carries the same three facts (faulting module,
    fault offset, exception code), and is where U21's own evidence came from in the first place.
    So the event log is primary; the file pull runs after it in case a box has WER on.

    Either way the record goes through crash_report's own offset+image-base -> docs/symbols.md
    resolution, so a VM crash names a FUNCTION and reads identically to a local one.
    """
    try:
        import crash_report
    except Exception as e:  # a diagnostic must never be the thing that fails the run
        return ["  [crash] crash_report unavailable for %s: %s" % (ip, e)]

    lines = []
    ev = remote(
        args,
        ip,
        "wevtutil qe Application /c:%d /rd:true /f:text /q:*[System[(EventID=1000)]]" % (limit * 8),
        timeout=40,
    )
    for r in crash_report.parse_appcrash_events(
        getattr(ev, "stdout", "") or "", since=since, app_path=args.vm_dir
    )[:limit]:
        lines.append("  [crash] " + crash_report.describe(r))
        lines.append("  [crash] source: %s on %s" % (r["report"], ip))
    if lines:
        return lines
    # No event said anything. Try the files too -- a box with WER enabled has more detail in them
    # (loaded modules, the full signature set), and this costs one `dir` on a box that has none.
    try:
        listing = remote(
            args,
            ip,
            " & ".join('dir /b /s /o-d "%s\\Report.wer" 2>nul' % r for r in VM_WER_ROOTS),
            timeout=40,
        )
    except Exception as e:  # a diagnostic must never be the thing that fails the run
        return ["  [crash] could not list WER reports on %s: %s" % (ip, e)]
    paths = [ln.strip() for ln in (getattr(listing, "stdout", "") or "").splitlines()]
    paths = [q for q in paths if q.lower().endswith("report.wer")][: limit * 4]
    if not paths:
        return []
    stage = os.path.join(_scratch(), "wer_%s" % ip.replace(".", "_"))
    shutil.rmtree(stage, ignore_errors=True)  # a previous run's report must not answer for this one
    pulled = 0
    for n, q in enumerate(paths):
        # Keep the report's own directory name: crash_report globs <root>/*/Report.wer, and that
        # directory is also what distinguishes two reports pulled in the same second.
        dest = os.path.join(stage, os.path.basename(os.path.dirname(q)) or ("r%d" % n))
        os.makedirs(dest, exist_ok=True)
        if (
            mp_run.scp(
                args.ssh_key,
                "%s@%s:%s" % (args.vm_user, ip, q.replace("\\", "/")),
                os.path.join(dest, "Report.wer"),
            ).returncode
            == 0
        ):
            pulled += 1
    if not pulled:
        return []
    return crash_report.report_for(args.vm_dir, since, limit=limit, roots=[stage])


def peer_captures(args, ip, run):
    return pull_local_captures(run) if ip is None else pull_remote_captures(args, ip, run)


def local_lan_ip():
    """This box's LAN address, as a VM peer would reach it.

    A UDP connect() to an off-box address picks the interface the routing table would use without
    sending anything -- more reliable than gethostbyname(gethostname()), which on a machine with
    Hyper-V switches happily returns a virtual adapter no VM can route back to.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("8.8.8.8", 53))
        return s.getsockname()[0]


# ---- net_shim orchestration (link-condition scenarios) ------------------------------------------
# A scenario that needs latency or a dying link needs `tools/net_shim.py` sitting between the peers.
# Starting it by hand does not survive contact: on 2026-07-26 a shim left running from a previous run
# still owned port 6501 with its blackhole latched ON, the replacement failed to bind, and the next
# run's client talked to the CORPSE -- a silently invalid result rather than an error. So the runner
# owns the lifetime: refuse to start on an occupied port, and always kill what we started.
def resolve_headless(args, ap):
    """Headless on unless --visible -- EXCEPT for runs that measure time.

    A pacing or determinism run is the one thing headless must not silently touch: with no blit there is
    no vsync wait, and the frame rate that mp_pacing_report.py and the adaptive lookahead controller
    measure goes from ~60 fps to ~8500. So those runs opt back into the blit.

    An explicit --headless is an INSTRUCTION and is refused (with --force-headless to override); the
    default is only an assumption, so it is corrected out loud instead of aborting a run nobody
    mis-specified.
    """
    if args.visible:
        return False
    timed = args.determinism or getattr(args, "ship_pacing", False)
    if timed and not args.force_headless:
        if args.headless:
            ap.error(
                "--headless with --determinism/--ship-pacing: no blit means no vsync wait (~60 fps -> "
                "~8500), which is what the pacing controller measures. Pass --force-headless if you "
                "really mean it."
            )
        print("[rig] pacing/determinism run -- keeping the blit (headless default suspended)")
        return False
    return True


_DESKTOP_HELD = False


def hold_desktop_once():
    """Create + hold the isolated desktop on first local launch. Idempotent per process."""
    global _DESKTOP_HELD
    if _DESKTOP_HELD:
        return
    desktop.hold(DESKTOP)
    _DESKTOP_HELD = True
    print("[rig] isolated desktop: %s (window/input namespace separate from yours)" % DESKTOP)


def resolve_desktop(args):
    """Isolated desktop unless the caller wants to WATCH the run.

    `--visible` means "put it on my screen", and an isolated desktop is precisely where you cannot see
    it -- so the two contradict and --visible wins. An EXPLICIT `--desktop` is an instruction and is
    honoured over that (you may well want a blit-on run parked away from you), but it is worth a line
    so nobody wonders where their window went.

    SCOPE: this only affects LOCAL launches. `test_ui.py --determinism` builds its peers from
    `args.vms` and runs entirely on the VM peers, so the flag reaches it but does nothing -- and it
    was never a local focus thief to begin with. Corrected here after 5ac09a3/59f99ff claimed
    otherwise in their messages.

    Same shape as resolve_headless, deliberately: default is an ASSUMPTION and is corrected out loud;
    an explicit flag is an INSTRUCTION and is obeyed.
    """
    if args.no_desktop:
        return ""
    if args.desktop:
        if args.visible:
            print(
                "[rig] --visible with an explicit --desktop: the window is on desktop %r, not yours"
                % args.desktop
            )
        return args.desktop
    if args.visible:
        return ""
    return desktop.DEFAULT_DESKTOP


def shim_listen_port(args):
    """Where the shim ACCEPTS peer connections.

    On the VM topology this is the game port itself: the shim runs on the dev box and the host is on a
    VM, so listening on 6501 here collides with nothing. LOCALLY the host lane is on THIS box and binds
    that very port, so a shim on the same number is a straight collision -- and the shim starts first,
    so it is the GAME that loses it: `net: bind(:6600) failed 10013`, no host listening, and the client
    times out on `sessions 1` looking like a discovery bug (H3, measured 2026-07-28). So the local path
    hands the shim its own port and puts the CLIENT lane on it (`[net] port` is the port a client
    dials), leaving the host lane's port to the host.
    """
    return args.shim_listen_port or args.port


def shim_lane_port_mismatch(args, client_dirs):
    """mp:D30 O5 -- a LOCAL LANE client dials the port in its OWN lane.json, not the shim's.

    A client lane provisioned at the HOST's port therefore bypasses the shim entirely, and the run is
    a LAN run printed as a "180 ms" one: three promoted-build runs came back IDENTICAL at srtt 0 (the
    lookahead on its 60 ms floor) before this was caught. Returns the refusal text, or "" when every
    local client lane dials the shim."""
    want = shim_listen_port(args)
    for pdir in client_dirs:
        if not pdir:
            continue
        try:
            with open(os.path.join(pdir, "lane.json"), "r", encoding="utf-8") as fh:
                lport = int(json.load(fh).get("port") or 0)
        except (OSError, ValueError):
            continue
        if lport and lport != want:
            return (
                "REFUSED: client lane %s dials port %d (its lane.json) but the shim listens on %d -- "
                "the client would bypass the shim and this would be a LAN run. Provision the client "
                "lane with --port %d." % (pdir, lport, want, want)
            )
    return ""


# ---- TL-RIG6: the shim must die with its runner --------------------------------------------------
# net_shim.py is a separate process Popen'd below. Before this fix nothing reaped it except a clean
# shim_stop() call, so a runner killed by the per-test timeout (test_ui.py's run_ui_test() ->
# subprocess.run(..., timeout=...), which on TimeoutExpired kills ui_test.py directly via
# Popen.kill()/TerminateProcess and never runs any of ui_test.py's own cleanup) orphaned the shim,
# which then held its port forever ("[shim] port 6714 is already in use" on the next run -- the
# 03:49 F5H gate leaked 6714, still held at 04:01). TWO independent mechanisms, because either one
# alone leaves a gap:
#  (1) a Windows Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, assigned to the shim right
#      after Popen. Windows closes every handle a process owns when that process exits BY ANY
#      MEANS (including TerminateProcess), so the job's kill-on-close fires the moment ui_test.py
#      is gone, with no cooperation from ui_test.py's own exit path needed. This is the forward
#      fix -- it makes future leaks not happen.
#  (2) a pidfile-based stale-holder breaker (the boot_lock pattern in make_lane.py), for anything
#      that leaked BEFORE this fix existed, or if job-object assignment itself fails (e.g. no
#      permission): shim_start(), on a bind() refusal, checks whether the holder is OUR OWN shim
#      from a runner that is now provably dead (its pid either doesn't exist any more, or has been
#      recycled onto an unrelated process -- checked via GetProcessTimes creation-time, which a PID
#      reuse cannot fake) and, only then, kills it and retries once. A holder whose runner is still
#      alive is left strictly alone -- this must never kill a live runner's shim.
#
# tooling:TL-SUITE-TEARDOWN generalised this mechanism to every other local child the rig owns (the
# game peers below, tools/desktop.py's isolated-desktop path, test_ui.py's relay/shim) -- the
# process-liveness primitives and the job-object code itself now live in tools/win_job.py so there
# is exactly one copy; these names stay as the local vocabulary this file's comments already use.
_process_still_running = win_job.process_still_running
_process_created_at = win_job.process_created_at
_process_terminate = win_job.process_terminate
_shim_kill_on_close_job = win_job.assign_kill_on_close


def _shim_pidfile_path(listen_port):
    return os.path.join(REPO, "tmp", "shim", "net_shim_%d.pid" % listen_port)


def _shim_write_pidfile(listen_port, shim_pid):
    os.makedirs(os.path.join(REPO, "tmp", "shim"), exist_ok=True)
    runner_pid = os.getpid()
    record = {
        "shim_pid": shim_pid,
        "runner_pid": runner_pid,
        "runner_started": _process_created_at(runner_pid),
        "port": listen_port,
        "written": time.time(),
    }
    try:
        with open(_shim_pidfile_path(listen_port), "w", encoding="utf-8") as fh:
            json.dump(record, fh)
    except OSError:
        pass


def _shim_reap_if_orphaned(listen_port):
    """A bind() to our own shim's port just failed -- before refusing, check whether the holder is
    OUR OWN shim (identified by the pidfile shim_start wrote for this port) from a runner that is
    now DEAD. Returns True (and has already killed the stale shim) iff the caller should retry the
    bind once; False means either there is no pidfile to go on, or the runner recorded in it is
    still alive -- in both cases the holder is left untouched."""
    try:
        with open(_shim_pidfile_path(listen_port), "r", encoding="utf-8") as fh:
            rec = json.load(fh)
    except (OSError, ValueError):
        return False
    runner_pid, shim_pid = rec.get("runner_pid"), rec.get("shim_pid")
    if not runner_pid or not shim_pid:
        return False
    # _process_still_running, NOT a bare OpenProcess/_process_created_at check: OpenProcess can
    # succeed on an ALREADY-EXITED pid for as long as anyone, anywhere, still holds a handle to it --
    # offline verification (TL-RIG7's scratch repro) caught this turning "is it running" into "does
    # the object still exist", which is a different question and the wrong one here.
    running = _process_still_running(runner_pid)
    if running is True:
        now_started = _process_created_at(runner_pid)
        recorded_started = rec.get("runner_started")
        if (
            now_started is not None
            and recorded_started is not None
            and abs(now_started - recorded_started) < 2.0
        ):
            return False  # same process, still running: a LIVE runner's shim -- never touch it
    print(
        "[shim] port %d is held by an orphaned shim (pid %s) whose runner (pid %s) is no longer "
        "the process that started it (TL-RIG6) -- reaping it and retrying once"
        % (listen_port, shim_pid, runner_pid)
    )
    _process_terminate(shim_pid)
    time.sleep(1.0)
    try:
        os.remove(_shim_pidfile_path(listen_port))
    except OSError:
        pass
    return True


def shim_start(args):
    if not args.shim:
        return None
    listen_port = shim_listen_port(args)
    # mp:TL-SHIMUDP -- the occupancy probe has to match the transport it is checking. TCP and UDP are
    # independent port spaces, so a TCP bind() here says nothing about a UDP shim already holding
    # this port: a stale `--udp` shim from an earlier run would sail past this check, and the run that
    # then dials it inherits a corpse with whatever delay/blackhole it last had -- silently invalid
    # rather than refused (the exact failure this probe exists to catch, one paragraph below).
    probe_kind = socket.SOCK_DGRAM if RUN_TRANSPORT == "udp" else socket.SOCK_STREAM
    # TL-RIG6: retry ONCE, and only if the holder can be positively identified as our own shim from
    # a now-dead runner (_shim_reap_if_orphaned) -- never on the first bind failure, and never past
    # one retry (a live runner's shim must come back False forever, not get killed on attempt N).
    for attempt in range(2):
        try:
            with socket.socket(
                socket.AF_INET, probe_kind
            ) as probe:  # fail LOUDLY, not inherit a stale shim
                probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                probe.bind(("0.0.0.0", listen_port))
            break
        except OSError:
            if attempt == 0 and _shim_reap_if_orphaned(listen_port):
                continue
            print(
                "[shim] %s port %d is already in use -- a shim from an earlier run is probably still\n"
                "       alive (and may have a blackhole latched on). Kill it and retry; a run through\n"
                "       a stale shim looks like a result but is not one."
                % (RUN_TRANSPORT, listen_port)
            )
            return None
    # The control port is TCP whatever the transport, and it is the singleton that used to force
    # every shim row onto one worker -- probe it too, so a collision is a named refusal rather than
    # the shim's own "[shim] failed to start (exit 1)".
    control_port = getattr(args, "shim_control_port", None) or SHIM_CONTROL_PORT
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        try:
            probe.bind(("127.0.0.1", control_port))
        except OSError:
            print(
                "[shim] control port %d is already in use -- another shim (or a stale one) holds it.\n"
                "       Two concurrent shims need distinct --shim-control-port values."
                % control_port
            )
            return None
    os.makedirs(os.path.join(REPO, "tmp", "shim"), exist_ok=True)
    # Per LISTEN port: two concurrent shims used to write (and delete) the same log file.
    log = os.path.join(REPO, "tmp", "shim", "ui_test_shim_%d.log" % listen_port)
    if os.path.exists(log):
        os.remove(log)
    cmd = [
        sys.executable,
        "-u",
        os.path.join(REPO, "tools", "net_shim.py"),
        "--listen",
        "0.0.0.0:%d" % listen_port,
        "--target",
        args.shim if ":" in args.shim else "%s:%d" % (args.shim, args.port),
        "--delay",
        str(args.shim_delay),
        "--jitter",
        str(args.shim_jitter),
        "--log",
        log,
        "--control",
        str(control_port),
    ]
    # mp:TL-SHIMUDP -- this used to be TCP unconditionally, so every `[net] transport=udp` run had to
    # be shimmed by hand (start net_shim.py --udp yourself, point ui_test at it): nothing here ever
    # told the shim which mode to forward in. RUN_TRANSPORT is resolved once in main() before either
    # caller of shim_start runs, so it is always current by the time we get here.
    manual_arm = False
    if RUN_TRANSPORT == "udp":
        cmd.append("--udp")
        if args.shim_timeline:
            # UDP has no connection state, so "first datagram" (the timeline's normal zero point) is
            # any stray packet that happens to land on this port before the real peer does -- an OS
            # broadcast, a leftover socket from a previous run, anything. This runner DOES know the
            # right zero point (the host it just confirmed LISTENING), so it arms the shim explicitly
            # instead of trusting whatever arrives first. See tools/net_shim.py --manual-arm / `arm`.
            cmd.append("--manual-arm")
            manual_arm = True
    if args.shim_timeline:
        t = args.shim_timeline
        cmd += ["--timeline", t if os.path.isabs(t) else os.path.join(REPO, t)]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    time.sleep(1.5)
    if proc.poll() is not None:
        print("[shim] failed to start (exit %s) -- see %s" % (proc.returncode, log))
        return None
    proc.manual_arm = manual_arm  # read by shim_arm() once the caller knows the host is ready
    proc.control_port = control_port  # read by shim_arm()
    proc.log_path = log  # read by shim_stop()
    proc.listen_port = listen_port  # read by shim_stop() (TL-RIG6 pidfile cleanup)
    _shim_write_pidfile(listen_port, proc.pid)  # TL-RIG6: so a future orphan can be identified
    proc.rig6_job = _shim_kill_on_close_job(proc.pid)  # TL-RIG6: dies with THIS process, any exit
    print(
        "[shim] listening :%s -> %s (%s)  delay=%sms one-way (%sms rtt)  control :%d%s"
        % (
            listen_port,
            args.shim if ":" in args.shim else "%s:%d" % (args.shim, args.port),
            RUN_TRANSPORT,
            args.shim_delay,
            args.shim_delay * 2,
            control_port,
            ("  timeline=" + os.path.basename(args.shim_timeline)) if args.shim_timeline else "",
        )
    )
    return proc


def shim_arm(proc):
    """mp:TL-SHIMUDP -- tell a --manual-arm shim its timeline clock starts NOW.

    Call this once the caller has confirmed the real session began (the host is LISTENING), not on a
    fixed delay -- that is the whole point of manual-arm over the datagram-triggered default. A no-op
    for a shim that was not started --manual-arm (proc.manual_arm is only set True by shim_start when
    RUN_TRANSPORT is udp and a timeline was requested).
    """
    if not proc or not getattr(proc, "manual_arm", False):
        return
    try:
        port = getattr(proc, "control_port", SHIM_CONTROL_PORT)
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.sendall(b"arm\n")
            s.settimeout(5)
            reply = s.recv(4096)
        print("[shim] %s" % reply.decode("utf-8", "replace").strip())
    except OSError as e:
        print(
            "[shim] WARN: could not arm the timeline clock (%s) -- the shim was started --manual-arm, "
            "so its timeline will now never start; the run's delay/jitter still apply, only a "
            "scheduled mid-run CHANGE (a timeline step) will not fire" % e
        )


def shim_ctl(proc, cmd):
    """Send one control-channel command to a shim this runner started; returns the reply or None."""
    port = getattr(proc, "control_port", SHIM_CONTROL_PORT)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.sendall((cmd.strip() + "\n").encode("utf-8"))
            s.settimeout(5)
            return s.recv(4096).decode("utf-8", "replace").strip()
    except OSError as e:
        print("[shim] WARN: control command %r failed (%s)" % (cmd, e))
        return None


# ---- EVIDENCE-BOUNDED shim actions (gate diet block 2, 2026-09-24) ------------------------------
# A shim TIMELINE is a wall clock, so a scenario that must act "after the evidence exists" had to
# guess the offset and add margin -- net_hud blackholed at t+300 s for a capture done at ~40 s,
# resync_storm_repro soaked 340 s for a storm whose 5th barrier lands 10 s into the match. A
# trigger is the state-gated form: `--shim-trigger JSON`, where JSON is
#   {"cmd": "<net_shim control command>", "when": [[<peer>, <file>, <regex>], ...]}
# <peer> is "host" or "c1".."cN" (launch order), <file> a log name the peer writes (mh_uidrive.log,
# mh_net.log, ...) looked up in its process dir AND every later dir of the same lane's logs/ (the
# session dirs), <regex> a Python regex searched in it. When EVERY condition holds the command is
# sent once, on the runner's 3 s poll. A timeline stays valid beside a trigger -- as the FALLBACK
# bound (the trigger's evidence never appearing is itself a finding the timeline then records).
# LOCAL peers only: a VM peer's condition is never satisfied (said once), so the timeline decides.
def parse_shim_triggers(raw):
    out = []
    for r in raw or []:
        t = json.loads(r)
        if not isinstance(t, dict) or not t.get("cmd") or not t.get("when"):
            raise ValueError(
                "--shim-trigger wants {\"cmd\": ..., \"when\": [[peer, file, regex], ...]}: %r" % r
            )
        conds = []
        for c in t["when"]:
            peer, fname, rx = c
            conds.append((peer, fname, re.compile(rx)))
        out.append({"cmd": t["cmd"], "when": conds, "fired": False})
    return out


def _peer_by_role(peers, role):
    if role == "host":
        return peers[0] if peers else None
    if role.startswith("c") and role[1:].isdigit():
        i = int(role[1:])
        return peers[i] if 0 < i < len(peers) else None
    return None


def _local_peer_file_texts(run, fname):
    """The texts of `fname` in the peer's process dir and every LATER dir of the same logs/ folder
    (dir names are UTC-timestamp-prefixed, so a lexical >= is 'created at or after')."""
    if not run or not os.path.isdir(run):
        return []
    logs = os.path.dirname(os.path.abspath(run).rstrip("\\/"))
    base = os.path.basename(os.path.abspath(run).rstrip("\\/"))
    out = []
    for d in sorted(os.listdir(logs)):
        if d < base:
            continue
        fp = os.path.join(logs, d, fname)
        if os.path.isfile(fp):
            try:
                with open(fp, encoding="utf-8", errors="replace") as fh:
                    out.append(fh.read())
            except OSError:
                pass
    return out


def pump_shim_triggers(proc, triggers, peers, t0):
    if not proc or not triggers:
        return
    for trg in triggers:
        if trg["fired"]:
            continue
        ok = True
        for role, fname, rx in trg["when"]:
            p = _peer_by_role(peers, role)
            if not p or not p.get("run"):
                ok = False
                break
            if p.get("ip") is not None:
                if not trg.get("warned"):
                    trg["warned"] = True
                    print(
                        "[shim] trigger %r: peer %s is not local -- cannot read its logs; the "
                        "timeline (if any) is the only bound" % (trg["cmd"], role)
                    )
                ok = False
                break
            if not any(rx.search(t) for t in _local_peer_file_texts(p["run"], fname)):
                ok = False
                break
        if ok:
            trg["fired"] = True
            if trg["cmd"].startswith("signal "):
                # A script signal instead of a shim command: drop rig_<name>.flag for every LOCAL
                # peer, the same file the signal ferry drops, so a script can `awaitsignal <name>`
                # on an evidence line it has no op to read (resync_countinit_proof ends on its
                # second barrier fire this way rather than on a game-clock window).
                name = trg["cmd"].split(None, 1)[1].strip()
                for p in peers:
                    if p.get("run") and p.get("ip") is None:
                        deliver_signal(None, p, name)
                reply = "signal %r delivered" % name
            else:
                reply = shim_ctl(proc, trg["cmd"])
            print(
                "[shim] TRIGGER at +%.0fs: %r -> %s  (when %s)"
                % (
                    time.time() - t0,
                    trg["cmd"],
                    reply,
                    "; ".join("%s:%s~/%s/" % (r, f, x.pattern) for r, f, x in trg["when"]),
                )
            )


def shim_stop(proc):
    if not proc:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
    # TL-RIG6: a clean stop means the pidfile is no longer current -- remove it so a LATER run does
    # not read a stale (but now-harmless-looking) record and skip the reap check for a genuinely
    # orphaned holder on the same port. The job handle can simply be closed: the process it protected
    # is already gone (we just waited for it), so closing does not trigger a kill of anything.
    lp = getattr(proc, "listen_port", None)
    if lp is not None:
        try:
            os.remove(_shim_pidfile_path(lp))
        except OSError:
            pass
    win_job.close_job(getattr(proc, "rig6_job", None))
    log = getattr(proc, "log_path", "")
    if log and os.path.exists(log):
        print("[shim] stopped; event log:")
        with open(log, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if any(
                    k in line
                    for k in ("OPEN", "CLOSE", "timeline", "STALL", "BLACKHOLE", "CUT", "SET")
                ):
                    print("    " + line.rstrip())


def _scratch():
    d = os.path.join(REPO, "tmp", "ui_test")
    os.makedirs(d, exist_ok=True)
    return d


# ---- capture collection + diff --------------------------------------------------------------------


def load_ignore(base_dir):
    """Optional baselines/<label>/_ignore.json: {"capture_x.png":[[x,y,w,h],...], "*":[...]} -> per-capture
    ignore rects (machine-specific regions like the on-screen host IP). Missing/broken file -> no masking.
    Keys starting with "_" are options, not captures: "_why" (prose), "_only" (compare ONLY a rect,
    optionally colour-blind -- see diff_capture)."""
    path = os.path.join(base_dir, "_ignore.json")
    if not os.path.isfile(path):
        return {}
    try:
        with open(path) as f:
            return json.load(f)
    except Exception as e:
        print("  [ignore] WARN: %s unreadable (%s) -- no masking" % (path, e))
        return {}


def collect_and_check(label, png_dir, args):
    """png_dir holds the pulled capture_<name>.png files. Diff each vs baselines/<label>/. Returns ok."""
    base_dir = os.path.join(BASELINES, label)
    caps = sorted(glob.glob(os.path.join(png_dir, "capture_*.png")))
    if not caps:
        print("  [%s] NO CAPTURES produced" % label)
        return False
    if args.update_baselines:
        os.makedirs(base_dir, exist_ok=True)
        for c in caps:
            shutil.copy(c, os.path.join(base_dir, os.path.basename(c)))
        print(
            "  [%s] wrote %d baselines -> %s" % (label, len(caps), os.path.relpath(base_dir, REPO))
        )
        return True
    ignore = load_ignore(base_dir)
    ok = True
    for c in caps:
        name = os.path.basename(c)
        rects = list(ignore.get("*", [])) + list(ignore.get(name, []))
        # `_only` (see diff_capture): {"capture_x.png": {"rect": [x, y, w, h], "mode": "ink"}}
        only = (ignore.get("_only") or {}).get(name)
        passed, frac, note = diff_capture(
            c, os.path.join(base_dir, name), args.pixdelta, args.tol, rects, only
        )
        tag = "PASS" if passed else "FAIL"
        print(
            "  [%s] %-28s %s  (%.3f%% diff%s)"
            % (label, name, tag, frac * 100, "; " + note if note else "")
        )
        ok = ok and passed
    return ok


def host_listening(port, transport="tcp"):
    # The menu-path host opens its lobby port when it enters its lobby -- the "ready for clients" signal
    # (used instead of script COMPLETE, since a host that waits for peers only COMPLETEs after they join).
    # Which TABLE that shows up in is transport-specific (mp:T1): a tcp host is a LISTEN row in the TCP
    # connection table, a udp host is a bound endpoint and has no state to be in at all. Asking the
    # wrong table is indistinguishable from a host that never started -- see remote_listening.
    q = (
        "(Get-NetUDPEndpoint -LocalPort %d -ErrorAction SilentlyContinue | Measure-Object).Count"
        % port
        if transport == "udp"
        else "(Get-NetTCPConnection -State Listen -LocalPort %d -ErrorAction SilentlyContinue | "
        "Measure-Object).Count" % port
    )
    r = mp_run.ps(q)
    try:
        return int((r.stdout or "0").strip() or "0") > 0
    except ValueError:
        return False


def pull_local_captures(run_dir):
    # The staging dir must be keyed by the LANE as well as the run stamp. The stamp has one-second
    # resolution, so several tests launched in the same second (routine under --jobs) produced the
    # SAME name and poured their captures into one directory -- the compare then diffed each test
    # against the union of everybody's frames. Measured 2026-07-28: esc_menu failed on four captures
    # that belong to debug_overlay and res_picker. Concurrency did not break the compare; it revealed
    # that the key was never unique.
    lane = os.path.basename(os.path.dirname(os.path.dirname(os.path.abspath(run_dir)))) or "local"
    png_dir = os.path.join(_scratch(), "host_%s_%s" % (lane, os.path.basename(run_dir)))
    os.makedirs(png_dir, exist_ok=True)
    for bmp in glob.glob(os.path.join(run_dir, "capture_*.bmp")):
        try:
            bmp_to_png(
                bmp, os.path.join(png_dir, os.path.splitext(os.path.basename(bmp))[0] + ".png")
            )
        except Exception as e:
            print("  (skip unreadable %s: %s)" % (os.path.basename(bmp), e))
    return png_dir


def pull_remote_captures(args, ip, run):
    png_dir = os.path.join(_scratch(), "client_%s_%s" % (ip.replace(".", "_"), run))
    os.makedirs(png_dir, exist_ok=True)
    listing = remote(args, ip, "dir /b %s\\logs\\%s\\capture_*.bmp 2>nul" % (args.vm_dir, run))
    for fn in listing.stdout.split():
        fn = fn.strip()
        if not fn.endswith(".bmp"):
            continue
        local_bmp = os.path.join(png_dir, fn)
        mp_run.scp(
            args.ssh_key,
            "%s@%s:%s/logs/%s/%s" % (args.vm_user, ip, args.vm_dir.replace("\\", "/"), run, fn),
            local_bmp,
        )
        if os.path.isfile(local_bmp):
            try:
                bmp_to_png(local_bmp, os.path.splitext(local_bmp)[0] + ".png")
            except Exception as e:
                print("  (skip unreadable %s: %s)" % (fn, e))
    return png_dir


# ---- UI-path determinism (mp_analyze over harness logs from a real UI-driven run) ------------------


def resolve_script(name):
    return name if os.path.isabs(name) or os.path.isfile(name) else os.path.join(UISCRIPTS, name)


# Stock `[video] size_mode` -> the view size the game applies (seams/video.cpp's table). Mode 4 only
# exists with the resolution picker armed; modes it does not list are left unchecked rather than guessed.
SIZE_MODES = {0: (640, 480), 1: (800, 576), 2: (1024, 768), 4: (1280, 800)}


def pinned_view_size(ini_text):
    """The view size an ini fragment PINS, or None if it pins none. Pure, so the guard is testable.

    A custom width/height wins over size_mode -- video.cpp forces size_mode=2 when a custom size is
    set, so the mode number no longer describes the view.
    """
    mode, cw, ch = None, None, None
    in_video = False
    for raw in ini_text.splitlines():
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        if line.startswith("["):
            in_video = line.lower().startswith("[video]")
            continue
        if not in_video or "=" not in line:
            continue
        k, v = (p.strip().lower() for p in line.split("=", 1))
        try:
            n = int(v, 0)
        except ValueError:
            continue
        if k == "size_mode":
            mode = n
        elif k == "width":
            cw = n
        elif k == "height":
            ch = n
    if cw and ch:
        return (cw, ch)
    return SIZE_MODES.get(mode)


def view_exceeds_desktop(view, desktop):
    """(view, desktop) -> True if the pinned view cannot fit the desktop. Pure.

    THIS GUARD EXISTS BECAUSE THE FAILURE IS SILENT AND EXPENSIVE (2026-08-20).
    A desktop NARROWER than the pinned mode does not produce "cannot set 1024x768" anywhere: the game
    launches, walks the whole menu, reaches `session_begin_multi`, serves ONE `[promote] time_tick`
    call and then never steps again, so the script sits on its next wait until the timeout. It reads
    exactly like a sim hang, and it cost a full day-long git bisect that came back "every commit is
    BAD" -- which it was, because the variable was never in git. The two suite tests that pin
    1024x768 failed and the nine that use the 640x480 default passed, on a 958x945 RDP desktop.
    """
    if not view or not desktop:
        return False
    return view[0] > desktop[0] or view[1] > desktop[1]


def primary_desktop_size():
    """The primary display's pixel size, or None where it cannot be read (non-Windows, no session)."""
    try:
        import ctypes

        user32 = ctypes.windll.user32
        try:
            user32.SetProcessDPIAware()  # else GetSystemMetrics returns the SCALED size
        except Exception:
            pass
        w, h = user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)
        return (w, h) if w > 0 and h > 0 else None
    except Exception:
        return None


def stall_abort(n, quiet_s, launch_timeout, stall_timeout):
    """Should a --determinism poll give up? Pure, so the decision can be checked without a rig.

    `n` is the host's logged step count as mp_run.steps_done reports it: -1 = no harness log yet,
    0 = log present but no step rows, >0 = stepping. `quiet_s` is how long that number has been
    unchanged. Two limits because "no steps yet" is normal during UI entry and a stall there means
    something different (the peers never rendezvoused) from a stall mid-run (the sim wedged).
    Either limit set to 0 disables that half.
    """
    limit = launch_timeout if n <= 0 else stall_timeout
    return bool(limit) and quiet_s > limit


def peer_liveness(run):
    """Is the LOCAL process behind `run` still alive, and did it leave a crash report?

    Returns (alive, lines). `alive` is None when we cannot tell -- a VM peer, or a run we did not
    launch -- and the caller must not read None as "dead".

    THIS IS THE DIFFERENCE BETWEEN A CRASH AND A HANG, and the rig could not tell them apart. When
    the sim thread faults the PROCESS usually survives: the render loop keeps presenting, the
    frametime log keeps growing, and a watchdog that only watches the step count reports
    `STALLED` -- the same word it uses for a livelock. On 2026-08-01 that cost an hour and produced
    two confident wrong diagnoses before anyone looked at the Windows crash report, which had named
    the faulting function all along (2026-08-01).

    So BOTH halves are checked, and they answer different questions:
      * the PID -- did the process exit? (a fault that does kill it)
      * WER    -- did anything fault, whether or not the process died? A crashed sim thread inside a
                  live process is invisible to the PID check, and it is the case that actually
                  happened here.
    Attribution is by AppPath against this peer's own lane directory, so concurrent lanes -- all
    running an exe named mh.focus.exe -- cannot be confused for each other.
    """
    meta = _RUN_META.get(run)
    pid = _RUN_PID.get(run)
    if meta is None:
        return None, []
    lane, t0 = meta
    rem = _RUN_REMOTE.get(run)
    if rem is not None:
        # A VM peer: no pid we own, no handle we could have retained. Same two questions, different
        # transport -- an image-name query on that box, and its own WER tree pulled back here.
        rargs, rip = rem
        return remote_alive(rargs, rip), remote_crash_lines(rargs, rip, t0)
    alive = None
    if pid:
        r = mp_run.ps(
            "if (Get-Process -Id %d -ErrorAction SilentlyContinue) { 'Y' } else { 'N' }" % pid
        )
        txt = (getattr(r, "stdout", "") or "").strip()
        if txt in ("Y", "N"):
            alive = txt == "Y"
    lines = []
    try:
        import crash_report

        lines = crash_report.report_for(lane, t0)
    except Exception as e:  # a diagnostic must never be the thing that fails the run
        lines = ["  [crash] crash_report unavailable: %s" % e]
    return alive, lines


def exit_witness(run):
    """The `; EXIT ...` line from this peer's own mh_net.log, if it left one. Lines to print.

    utils_abort -> _exit() and llm_wnd_on_destroy -> ExitProcess are the binary's two SELF-DRIVEN
    exits, both silent to WER by construction; mh.dll writes a run-before witness for each (D15,
    seams/net_diag.cpp). Absence is evidence ONLY once the arm banner is confirmed -- otherwise
    "no line" and "no instrument" read identically. For a local peer `run` IS the run dir
    (local_new_run); a VM peer's log lives on the VM and is out of this helper's reach.
    """
    if not run or run in _RUN_REMOTE or not os.path.isdir(run):
        return []
    log = os.path.join(run, "mh_net.log")
    try:
        txt = open(log, encoding="utf-8", errors="replace").read()
    except OSError as e:
        return ["  [exit] could not read %s: %s" % (log, e)]
    hits = [ln.strip() for ln in txt.splitlines() if "; EXIT " in ln]
    if hits:
        return ["  [exit] SELF-DRIVEN -- %s" % h for h in hits[-2:]]
    if "exit witness armed on utils_abort" not in txt:
        return [
            "  [exit] the exit witness was NEVER ARMED in this run -- silence here means "
            "nothing; do not read it either way (%s)" % log
        ]
    return [
        "  [exit] witness ARMED and SILENT -> not a self-driven exit; a real fault is the "
        "remaining explanation. NOTE: under --desktop (the default), WER cannot record it (G186)."
    ]


def postmortem_archive(run):
    """Copy a dead peer's text logs to tmp/ui_test/postmortem/ before make_lane can rmtree them.

    make_lane.py deletes the whole lane -- logs included -- on every provision, which is how three
    F5H gate crashes and one reproduction left zero evidence (2026-09-15). Archiving lives HERE, on
    the dead-peer path, not in make_lane: it costs nothing on a green run, and a blanket keep across
    ~145 lanes (2-20 MB of text per run) is a gigabyte this disk does not have. Text logs only,
    tail-capped -- captures are already PNG'd into tmp/ui_test by pull_local_captures.
    """
    if not run or run in _RUN_REMOTE or not os.path.isdir(run):
        return
    lane, _ = _RUN_META.get(run, (None, None))
    dst = os.path.join(
        REPO,
        "tmp",
        "ui_test",
        "postmortem",
        "%s__%s" % (os.path.basename(lane or "unknown"), os.path.basename(run)),
    )
    cap = 512 * 1024
    try:
        os.makedirs(dst, exist_ok=True)
        for f in glob.glob(os.path.join(run, "*.log")) + glob.glob(os.path.join(run, "*.txt")):
            with open(f, "rb") as src:
                src.seek(0, 2)
                size = src.tell()
                src.seek(max(0, size - cap))
                data = src.read()
            with open(os.path.join(dst, os.path.basename(f)), "wb") as out:
                out.write(data)
        print("  [postmortem] logs archived to %s" % dst)
    except OSError as e:  # a diagnostic must never be the thing that fails the run
        print("  [postmortem] archive failed: %s" % e)


def report_dead_peer(key, run):
    """Say that a peer's PROCESS is gone, and which way it went. True if it is.

    D15: "script TIMED-OUT (no marker)" is what the runner said for a peer that had already exited,
    and it is a budget-shaped sentence about a process that no longer existed -- the exact wording
    that sent two sessions after the wall-clock budgets. A dead peer cannot reach its marker, so the
    honest report is the death, and the exit code names its class.
    """
    alive, lines = peer_liveness(run)
    if alive is not False:  # None = cannot tell (a VM peer); never read that as dead
        return False
    print(
        "  [%s] the game PROCESS IS GONE -- it cannot reach a marker, so this is not a budget" % key
    )
    if run not in _RUN_REMOTE:  # the exit code needs a handle, which only a local launch retains
        print("  [%s] %s" % (key, describe_exit(peer_exit_code(_RUN_PID.get(run)))))
    # Read the witness ourselves instead of telling a human where it lives (the prose version of
    # this line went unread through three F5H investigations).
    for ln in exit_witness(run):
        print(ln)
    for ln in lines:
        print(ln)
    postmortem_archive(run)
    return True


def peer_verdict(run, stalled_status):
    """Turn a stall verdict into a CRASH verdict when the evidence says so, and print the evidence.

    Called at the moment the watchdog would have given up. A crash report attributable to this lane
    outranks the stall reading: the sim stopped stepping BECAUSE it faulted, and reporting the
    symptom over the cause is what sent the last investigation down a rabbit hole.
    """
    alive, lines = peer_liveness(run)
    for ln in lines:
        print(ln)
    if lines:
        return (
            stalled_status.replace("STALLED", "CRASHED", 1)
            if "STALLED" in stalled_status
            else ("CRASHED " + stalled_status)
        )
    if alive is False:
        # No WER report, but the process is gone. Also not a stall -- and worth saying that WER may
        # simply be disabled here, rather than implying nothing crashed.
        print("  [crash] the game process EXITED and no WER report was found for this lane")
        print(
            "  [crash] (a lane on the isolated mh_rig desktop -- the DEFAULT -- is INVISIBLE to "
            "WER: WerFault cannot run there, so this absence is NO INFORMATION, not 'no fault'. "
            "G186; re-run with --no-desktop to capture one)"
        )
        # WHICH exit, though. "The process is gone" is where both previous investigations stopped,
        # and it is equally compatible with a clean quit, a fault WER did not record, and this rig's
        # own kill -- three different bugs (D15 cause #2).
        print("  [exit] %s" % describe_exit(peer_exit_code(_RUN_PID.get(run))))
        for ln in exit_witness(run):
            print(ln)
        postmortem_archive(run)
        return stalled_status.replace("STALLED", "PROCESS-GONE", 1)
    return stalled_status


def peer_harness_steps(args, ip, run):
    """(steps_logged, done) from a peer's mh_harness.log -- read in place (local) or pulled (VM)."""
    if ip is None:
        return mp_run.steps_done(run)
    tmp = os.path.join(_scratch(), "det_poll_%s" % ip.replace(".", "_"))
    os.makedirs(tmp, exist_ok=True)
    fwd = args.vm_dir.replace("\\", "/")
    mp_run.scp(
        args.ssh_key,
        "%s@%s:%s/logs/%s/mh_harness.log" % (args.vm_user, ip, fwd, run),
        os.path.join(tmp, "mh_harness.log"),
    )
    return mp_run.steps_done(tmp)


def peer_script_abort(args, ip, run):
    """The peer's own `[script] TIMEOUT at step N ... ABORT: <step>` line, or None.

    DET-FLAKE shape (B): on 2026-09-10 the host's script gave up at 37 s on step 15 `peers 1` -- the
    client's handshake arrived 0.5 s later -- and the driver then sat out its full 300 s launch
    watchdog before reporting a bare "host step count stuck at 0". The log that said exactly what
    happened was sitting on the peer the whole time. A stalled launch should be named, not timed out.
    """
    if run is None:
        return None
    path = os.path.join(run, "mh_uidrive.log")
    if ip is not None:
        tmp = os.path.join(_scratch(), "det_abort_%s" % ip.replace(".", "_"))
        os.makedirs(tmp, exist_ok=True)
        path = os.path.join(tmp, "mh_uidrive.log")
        fwd = args.vm_dir.replace("\\", "/")
        if (
            mp_run.scp(
                args.ssh_key,
                "%s@%s:%s/logs/%s/mh_uidrive.log" % (args.vm_user, ip, fwd, run),
                path,
                quiet=True,
            ).returncode
            != 0
        ):
            return None
    try:
        with open(path, "r", errors="replace") as f:
            for ln in f:
                if "ABORT:" in ln and "TIMEOUT at step" in ln:
                    return ln.strip()
    except OSError:
        return None
    return None


# SES1: a SESSION directory, exactly as mh_session_dir.h spells it --
# "<UTC YYYYMMDDTHHMMSSZ>_<8 hex>_<slot>_<role>". A process ("menu") directory has `menu` where the
# hex is, so it cannot match; and neither can a pre-SES1 `<YYYYMMDD>_<HHMMSS>_<role>` folder, which
# is the one that actually bit (see peer_session_dirs).
SESSION_DIR_RE = re.compile(r"^\d{8}T\d{6}Z_[0-9a-f]{8}_\d+_[A-Za-z0-9]+$")


def peer_session_dirs(args, ip, run):
    """SES1: the SESSION directories that opened under the process directory `run`, oldest first.

    A run's output is two kinds of folder since SES1. `run` is the PROCESS ("menu") directory -- the
    one this runner discovered at launch and has been polling for mh_uidrive.log -- and it keeps the
    streams whose subject is the process: the harness outputs, the UI-automation channel, the
    captures, the arm-time banners. Everything whose subject is the MATCH (mh_net.log,
    mh_lockstep.log, mh_frametime.log, mh_launch.log) moves into a session directory the moment a
    lobby opens, and moves back out when it closes.

    Identified by NAME SHAPE plus ordering, rather than by reading each session.json: one directory
    listing answers it, where a json per candidate would be one round trip each on a VM.

    THE SHAPE TEST IS NOT BELT-AND-BRACES. The first version of this used only "sorts after `run`
    and is not a menu directory", and a rig VM whose logs/ still held PRE-SES1 folders
    (`20260917_155937_solo`) silently matched them: `_` is 0x5F and `T` is 0x54, so an old-format
    name from EARLIER the same day sorts AFTER a new-format one. The determinism gate then appended
    an unrelated 800-step run's harness log onto this run's, and came back NO COMPARABLE STEPS with
    two boot banners in one file. Matching the shape SES1 actually emits is what makes the ordering
    comparison meaningful, because both sides of it are then the same format."""
    base = os.path.basename(run.rstrip("/\\"))
    if ip is None:
        names = [
            os.path.basename(d.rstrip("/\\"))
            for d in glob.glob(os.path.join(os.path.dirname(os.path.abspath(run)), "*"))
            if os.path.isdir(d)
        ]
    else:
        r = remote(args, ip, "dir /b /ad %s\\logs 2>nul" % args.vm_dir)
        names = [ln.strip() for ln in (r.stdout or "").splitlines() if ln.strip()]
    return sorted(n for n in names if SESSION_DIR_RE.match(n) and n > base)


def pull_peer_logs(args, ip, run, dest):
    """Copy a peer's harness/net logs (mp_run.LOGNAMES) from its run dir into `dest`. Returns dest.

    SES1: `dest` stays FLAT and stays one file per log name, because every consumer downstream
    (mp_analyze, det_run_report, det3_barrier_report) reads it that way -- and because the flat file
    is what the pre-SES1 single directory produced. The process directory's copy is written first
    and each session directory's copy is APPENDED in stamp order, which reconstructs exactly the
    stream that used to exist: the menu phase, then each match, in the order they happened. The
    newest session's session.json comes along too, so the analyzer can pair these peers by match_id
    rather than by the operator having handed it the right two folders."""
    os.makedirs(dest, exist_ok=True)
    sessions = peer_session_dirs(args, ip, run)
    if ip is None:
        for nm in mp_run.LOGNAMES + OPTIONAL_ARTIFACTS:
            src = os.path.join(run, nm)
            if os.path.isfile(src):
                shutil.copy(src, os.path.join(dest, nm))
        parent = os.path.dirname(os.path.abspath(run))
        for sd in sessions:
            for nm in mp_run.LOGNAMES:
                src = os.path.join(parent, sd, nm)
                if os.path.isfile(src):
                    with open(src, "rb") as fi, open(os.path.join(dest, nm), "ab") as fo:
                        fo.write(fi.read())
            sj = os.path.join(parent, sd, "session.json")
            if os.path.isfile(sj):
                shutil.copy(sj, os.path.join(dest, "session.json"))
        return dest
    if True:
        # OPTIONAL artifacts first, quietly: they exist only under --record, so a missing one is the
        # normal case and must NOT print the "did NOT copy" line that a missing LOG legitimately does.
        fwd0 = args.vm_dir.replace("\\", "/")
        for nm in OPTIONAL_ARTIFACTS:
            # quiet=True is what makes the comment above TRUE: mp_run.scp prints its own "scp FAILED"
            # line, so without it this loop announced a failure for every run that simply was not a
            # --record run -- the loud message this branch exists to avoid, just emitted one level down.
            mp_run.scp(
                args.ssh_key,
                "%s@%s:%s/logs/%s/%s" % (args.vm_user, ip, fwd0, run, nm),
                os.path.join(dest, nm),
                quiet=True,
            )
    if ip is not None:
        # Retry once and SAY SO on failure. A silently-dropped pull is how a determinism run ends up
        # comparing one peer against nothing: 2026-07-28, the host's mh_harness.log (2.8 MB, present on
        # the VM) failed to copy while its frametime/temporal siblings succeeded, so the pair had no
        # host data and the run reported INCONCLUSIVE with no hint as to why. The --min-common floor did
        # its job -- it refused to call that a pass -- but a gate should also name what went missing.
        #
        # SES1 MOVED THE REPORT TO THE END. A stream that is per-SESSION is legitimately ABSENT from
        # the process directory -- mh_lockstep.log is written only in mode 3, so a peer's menu folder
        # never has one -- and warning here made the determinism gate print two "did NOT copy" lines
        # on every GREEN run, for files that arrived a few lines later out of the session folder. The
        # retry stays; the verdict on whether anything is missing is taken once, over the merge.
        fwd = args.vm_dir.replace("\\", "/")
        for nm in mp_run.LOGNAMES:
            src = "%s@%s:%s/logs/%s/%s" % (args.vm_user, ip, fwd, run, nm)
            out = os.path.join(dest, nm)
            for attempt in (1, 2):
                if mp_run.scp(
                    args.ssh_key, src, out, quiet=True
                ).returncode == 0 and os.path.isfile(out):
                    break
        # SES1: then each session directory's half of the same streams, appended in stamp order.
        # Pulled to a scratch name and concatenated rather than scp'd over the destination, so a
        # failed session pull cannot destroy the menu-phase half that already arrived.
        for sd in sessions:
            for nm in mp_run.LOGNAMES:
                tmp = os.path.join(dest, "_ses_%s" % nm)
                if mp_run.scp(
                    args.ssh_key,
                    "%s@%s:%s/logs/%s/%s" % (args.vm_user, ip, fwd, sd, nm),
                    tmp,
                    quiet=True,
                ).returncode == 0 and os.path.isfile(tmp):
                    with open(tmp, "rb") as fi, open(os.path.join(dest, nm), "ab") as fo:
                        fo.write(fi.read())
                    os.remove(tmp)
            mp_run.scp(
                args.ssh_key,
                "%s@%s:%s/logs/%s/session.json" % (args.vm_user, ip, fwd, sd),
                os.path.join(dest, "session.json"),
                quiet=True,
            )
        # THE ONE VERDICT ON WHAT IS MISSING, taken over the merged result (see the note above the
        # process-directory pull). 2026-07-28's failure -- a 2.8 MB mh_harness.log present on the VM
        # and silently absent locally, leaving the pair with no host data -- is still named here; what
        # is no longer named is a per-session stream that the process directory correctly lacks.
        for nm in mp_run.LOGNAMES:
            if not os.path.isfile(os.path.join(dest, nm)):
                print("  [pull] %s: %s did NOT copy -- analysis will be missing it" % (ip, nm))
    return dest


# ---- TL-HARN-CLEANCLOSE: the peers' own end of the match ---------------------------------------
# The harness closes each peer's match IN PLACE at its stop step (MH_Session_HarnessStop,
# net_discovery.cpp): the desync detector's rollup, the inbound-queue rollup + match boundary and
# SESSION_END, then this line. The runner still ends the processes from outside afterwards -- that is
# unchanged -- so what it must not do is PULL a peer before that peer reached its own stop step. The
# host is the peer this loop waits on, so it is always there; a client a few lockstep steps behind
# may not be, and a pull taken then would carry every hash but none of the end-of-match lines.
# Registered as net.harness_stop in tools/data/log_formats.json.
HARNESS_STOP = "; [session] HARNESS_STOP"
HARNESS_STOP_RE = re.compile(r"; \[session\] HARNESS_STOP step=(\d+) close=(\w+)")
# The wait is for the STRAGGLER only, and it is bounded well inside the +5 s a run may grow by: a
# client that is still behind after this is not "slow to close", it is behind the host by more
# than the lookahead, and the compared-step floor has its own word on that.
CLOSE_WAIT_S = 8


def harness_stop_of(peer_dir):
    """(step, close) from a pulled peer's mh_net.log's LAST HARNESS_STOP line, or None."""
    try:
        with open(os.path.join(peer_dir, "mh_net.log"), encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return None
    hits = HARNESS_STOP_RE.findall(text)
    return (int(hits[-1][0]), hits[-1][1]) if hits else None


def det_await_close(args, peers, det_dir, exclude):
    """Re-pull any peer whose pulled log has no HARNESS_STOP line yet, for up to CLOSE_WAIT_S.

    Returns {key: (step, close) or None}. Costs nothing when every peer already closed -- the
    common case, since the host's pull alone takes longer than a client's lag.

    ONLY A PEER STILL SHORT OF ITS STOP STEP IS WAITED FOR. The harness flushes its per-region
    breakdown and closes the match in the same breath, and pull_peer_logs copies mh_harness.log
    BEFORE mh_net.log -- so a pulled breakdown with no HARNESS_STOP beside it is a peer that will
    never write one (close_on_stop=0, or an mh.dll from before TL-HARN-CLEANCLOSE), and waiting on
    it would only spend the budget re-pulling a finished answer."""
    want = [(k, ip, run) for k, ip, run in peers if run and k not in (exclude or [])]
    got = {k: harness_stop_of(os.path.join(det_dir, k)) for k, _ip, _run in want}

    def straggling(k):
        return got[k] is None and not mp_run.steps_done(os.path.join(det_dir, k))[1]

    deadline = time.time() + CLOSE_WAIT_S
    while any(straggling(k) for k in got) and time.time() < deadline:
        time.sleep(1)
        for k, ip, run in want:
            if straggling(k):
                dest = os.path.join(det_dir, k)
                # pull_peer_logs APPENDS the session folders' halves, so a re-pull starts clean.
                shutil.rmtree(dest, ignore_errors=True)
                pull_peer_logs(args, ip, run, dest)
                got[k] = harness_stop_of(dest)
    print(
        "[det] clean close: %s"
        % ", ".join(
            "%s=%s" % (k, ("%s@%d" % (v[1], v[0])) if v else "ABSENT")
            for k, v in sorted(got.items())
        )
    )
    for k, v in sorted(got.items()):
        if v is None:
            print(
                "[det] %s wrote no `%s` line within %ds of the pull -- its end-of-match lines "
                "(desync rollup, queue rollup, SESSION_END) are not in this run's logs"
                % (k, HARNESS_STOP, CLOSE_WAIT_S)
            )
    return got


def run_determinism(args):
    """UI-PATH determinism: launch every peer through the REAL menu->lobby->Start (no force-entry), run
    `--steps` in-game with the [harness] region-hash logger active, then mp_analyze.py the peers' logs for
    ALL PAIRS IDENTICAL. This exercises the real menu/lobby/handoff code that force-entry mp_run bypasses."""
    if not args.host:
        print("[det] --determinism needs --host <ip>:<script> and >=1 --client <ip>:<script>")
        return 1
    steps = args.steps
    host_ip, hspec, hdir = parse_peer(args.host)
    hsrc = resolve_script(hspec)
    connect_ip = args.connect_ip or host_ip or args.host_ip
    clients = []
    for spec in args.client:
        cip, cspec, cdir = parse_peer(spec)
        if cip is None and not cdir:
            print("[det] a local --client must be 'lane=<name>:script' (it needs its own folder)")
            return 1
        clients.append((cip, resolve_script(cspec), cdir))
    if not clients:
        print("[det] --determinism needs >=1 --client")
        return 1

    print(
        "[det] UI-path determinism: %d steps, host=%s, %d client(s)"
        % (steps, host_ip or "local", len(clients))
    )
    # A determinism run through the shim is how a pacing experiment gets realistic latency (P4/P5) --
    # and it is also the strongest determinism test there is, since the sim must stay bit-identical
    # while the link is anything but ideal.
    shim = shim_start(args)
    if args.shim and not shim:
        return 1
    if shim:
        connect_ip = local_lan_ip()
        print("[shim] clients will connect to %s" % connect_ip)
        bad = shim_lane_port_mismatch(args, [cdir for _cip, _cs, cdir in clients])
        if bad:
            shim_stop(shim)
            print(bad)
            return 1
    peers = []  # (key, ip, run)
    pin_setup(args, host_ip, name=args.host_name, game=args.game_name, pdir=hdir)
    print(
        "[host %s] launching %s (harness %d steps) ..."
        % (host_ip or "local", os.path.basename(hsrc), steps)
    )
    hrun = peer_launch(
        args, host_ip, hsrc, args.timeout_frames, harness_steps=steps, is_host=True, pdir=hdir
    )
    if not hrun:
        peer_kill(args, host_ip)
        shim_stop(shim)  # mp:D30 O5: an early abort left the shim holding its port for the next run
        return 1
    peers.append(("host", host_ip, hrun))
    ready_deadline = time.time() + min(max(args.timeout, 90), 90)
    while time.time() < ready_deadline and not peer_ready(args, host_ip, args.port):
        time.sleep(2)
    if not peer_ready(args, host_ip, args.port):
        print(
            "[det] host never opened %d/%s within 90s -- aborting. If the polls above printed "
            "ssh timeouts, suspect the PROBE, not the game (2026-07-27 -- see remote_listening)."
            % (args.port, RUN_TRANSPORT)
        )
        peer_kill(args, host_ip)
        shim_stop(
            shim
        )  # mp:D30 O5: without this the next run is refused "udp port ... already in use"
        return 1
    shim_arm(
        shim
    )  # mp:TL-SHIMUDP -- the host is really up now; a manual-arm shim's clock starts here
    print(
        "[host %s] LISTENING -- launching clients (connect to %s)"
        % (host_ip or "local", connect_ip)
    )
    for ci, (cip, csrc, cdir) in enumerate(clients):
        cname = args.client_name if ci == 0 else "%s%d" % (args.client_name, ci + 1)
        pin_setup(args, cip, ip_val=connect_ip, name=cname, pdir=cdir)
        print(
            "[client %s] launching %s ..." % (cip or os.path.basename(cdir), os.path.basename(csrc))
        )
        crun = peer_launch(args, cip, csrc, args.timeout_frames, harness_steps=steps, pdir=cdir)
        peers.append(("client%d" % (ci + 1), cip, crun))

    # Wait for the HOST to log >= `steps` in-game steps, then stop everyone -- the peers run in lockstep so
    # a host at N steps means every peer logged the same N. NOTE: the DLL defaults [harness] stop_step in the
    # non-force-entry (UI) path (it does not self-stop / write the per-region breakdown), so we BOUND the run
    # by polling to n>=steps rather than waiting for `done`. Overshooting N just compares MORE steps (a
    # stronger check). Generous deadline: real UI entry (~30-60s) + the in-game steps.
    start_wait = time.time()
    deadline = start_wait + max(args.timeout, 120 + steps * 0.5)
    print(
        "[det] running %d in-game steps (waiting for the host to finish; do not touch any window) ..."
        % steps
    )
    # The determinism path drives the SAME match_launch scripts as the capture suite, so it needs the
    # same peer rendezvous -- mp_host_start waits on `awaitsignal lobby`. Wiring the ferry into only the
    # capture-suite wait loop left both peers parked in the menu forever (2026-07-28): the host waited
    # for a signal nothing was delivering. Two loops, one mechanism; keep them in step.
    sig_peers = [{"key": k, "ip": ip, "run": run} for (k, ip, run) in peers if run]
    delivered = set()
    # PROGRESS WATCHDOG (2026-07-29). `deadline` above is pure wall clock, and at 3000 steps it works
    # out to ~53 minutes -- so a WEDGED run and a merely slow one look identical for most of an hour.
    # A total-time bound cannot separate those two; a progress bound can, and the poll below already
    # has the signal (the host's step count). Lowering the wall clock instead would just start killing
    # healthy long runs. Two phases, because "no steps yet" is normal during UI entry:
    #   n <= 0  -- the harness log is absent (-1) or has no step rows yet: still launching, allow
    #              --launch-timeout (real UI entry is ~30-60 s, plus the peer rendezvous).
    #   n  > 0  -- stepping: any pause longer than --stall-timeout is a stall. Safe against a legitimate
    #              barrier park, which is bounded by rx_timeout_ms (10 s default) before the link drops.
    last_n, last_change, stalled = None, time.time(), False
    while time.time() < deadline:
        time.sleep(8)
        if len(sig_peers) > 1:
            pump_signals(args, sig_peers, delivered)
        n, done = peer_harness_steps(args, host_ip, hrun)
        print("    host=%s%s" % (n, " DONE" if done else ""))
        if done or n >= steps:
            break
        if n != last_n:
            last_n, last_change = n, time.time()
            continue
        quiet = time.time() - last_change
        limit = args.launch_timeout if n <= 0 else args.stall_timeout
        # DET-FLAKE (B): before spending the rest of the launch budget, ask the peers whether their
        # SCRIPT already gave up. A run whose host aborted at step 15 is not slow, it is over --
        # and the abort line names the step, which "step count stuck at 0" never could. Checked only
        # while nothing has stepped yet (n <= 0), and only every other poll, to keep the scp cheap.
        if n <= 0 and quiet >= 24:
            aborted = [(k, peer_script_abort(args, ip, run)) for k, ip, run in peers if run]
            aborted = [(k, ln) for k, ln in aborted if ln]
            if aborted:
                for k, ln in aborted:
                    print("[det] %s's SCRIPT ABORTED -- %s" % (k, ln))
                print(
                    "[det] the match never started, and the peer said so itself -- not waiting out "
                    "the remaining %ds of the launch watchdog"
                    % max(0, int(args.launch_timeout - quiet))
                )
                stalled = True
                break
        if stall_abort(n, quiet, args.launch_timeout, args.stall_timeout):
            print(
                "[det] STALL: host step count stuck at %s for %ds (%s limit %ds) -- aborting rather "
                "than waiting out the %ds wall clock"
                % (n, quiet, "launch" if n <= 0 else "stepping", limit, deadline - start_wait)
            )
            # Say WHY, when Windows recorded it. A local host that faulted looks identical to one
            # that wedged, and only one of the two is worth re-running unchanged. (VM peers return
            # nothing here -- their WER lives on the VM; see peer_liveness.)
            for ln in peer_liveness(hrun)[1]:
                print(ln)
            stalled = True
            break
    else:
        print(
            "[det] WARN: host did not reach %d steps before the deadline -- analyzing what exists"
            % steps
        )
    if stalled:
        # Fall through to the pull/analyze path anyway: the min_common floor below is what turns
        # "almost no steps compared" into a FAIL, so a stalled run is rejected on evidence rather than
        # on the abort alone -- and the partial logs are worth having.
        print("[det] (stalled run -- the compared-step floor decides the verdict)")

    det_dir = os.path.join(_scratch(), "determinism")
    if os.path.isdir(det_dir):
        shutil.rmtree(det_dir, ignore_errors=True)
    dirs = []
    for key, ip, run in peers:
        if run:
            pulled = pull_peer_logs(args, ip, run, os.path.join(det_dir, key))
            # mp:U19b -- a peer that LEAVES the match on purpose (the quitter of the 3-peer clean
            # quit) has a legitimately short hash log. Its logs are still pulled -- the shape's
            # post_check reads them -- but it takes no part in the all-pairs compare, which would
            # otherwise read its ~300 steps as "a peer produced no state hashes" and fail a run
            # whose two SURVIVORS compared every requested step.
            if key in (args.det_exclude or []):
                print(
                    "[det] %s pulled but excluded from the pairwise compare (--det-exclude)" % key
                )
                continue
            dirs.append(pulled)
    if not stalled:
        # TL-HARN-CLEANCLOSE: a straggling client's close lands a moment after the host's.
        det_await_close(args, peers, det_dir, args.det_exclude)
    for key, ip, run in peers:
        peer_kill(args, ip, run)
    shim_stop(shim)
    if len(dirs) < 2:
        print("[det] need >=2 peers with logs; got %d -- FAIL" % len(dirs))
        return 1
    print("[det] analyzing %d peer log dir(s) via mp_analyze.py ..." % len(dirs))
    # Require the pair to have actually compared most of the requested steps. Without a floor, a peer
    # that dies on launch contributes ZERO comparable steps, scores mismatch=0, and the run reports
    # "ALL PAIRS IDENTICAL" having verified nothing (observed 2026-07-25 after an intermittent client
    # crash: combined-hash=0). Half the requested steps is a deliberately loose floor -- it rejects the
    # vacuous case and short runs without flapping on the usual off-by-a-few overlap.
    min_common = max(1, int(args.steps) // 2)
    subprocess.run(
        [
            sys.executable,
            os.path.join(REPO, "tools", "mp_analyze.py"),
            "--min-common",
            str(min_common),
            *dirs,
        ]
    )
    clean, nodata, analyzer_verdict = None, 0, None
    try:
        with open(os.path.join(dirs[0], "mp_analyze.json")) as f:
            j = json.load(f)
        clean = j.get("all_pairwise_clean")
        nodata = j.get("pairs_without_data") or 0
        analyzer_verdict = j.get("verdict")
    except Exception:
        pass
    if analyzer_verdict:
        # SAY WHAT THE ANALYZER SAID. This used to re-derive a verdict from the flags, and the moment
        # DET-FLAKE's environmental guard landed the two disagreed: a run the analyzer deliberately
        # keeps as DESYNC (link evidence present but not conclusive -- its fail-closed rule) came back
        # from here as "FAIL: ENVIRONMENTAL", undoing that rule one layer up. One verdict, one author.
        verdict = analyzer_verdict
    elif clean:
        verdict = "ALL PAIRS IDENTICAL"
    elif nodata:
        verdict = (
            "NO COMPARABLE STEPS (a peer produced <%d hashed steps) -- NOT a pass" % min_common
        )
    elif clean is False:
        verdict = "DESYNC -- NOT clean"
    else:
        verdict = "INCONCLUSIVE (see mp_analyze output)"
    print("\n[det] VERDICT: %s" % verdict)

    # WHERE THE EVIDENCE LIVES. The local copies are transient (the next run wipes the live dir);
    # each peer VM keeps its own per-run folder, and that archive is the only reason the 2026-09-09
    # red could be re-analysed a day later. Print it on every run, not only on a red -- a run you
    # did not think was interesting is exactly the one you want the logs for later.
    print("[det] peer logs kept on each peer (they outlive the local copies):")
    for key, ip, run in peers:
        if run:
            print("        %-8s %s:%s/logs/%s" % (key, ip or "local", args.vm_dir, run))
    if not clean:
        # Keep the last three failures locally, so a red survives the next run's wipe.
        # basename: a LOCAL lane's run is a full path (F:\...\logs\<stamp>), which made the old
        # `determinism.red-F:\...` name invalid and lost every local red (mp:T4 O4, 2026-09-24).
        keep = os.path.join(
            _scratch(),
            "determinism.red-%s"
            % (os.path.basename(os.path.normpath(peers[0][2])) if peers[0][2] else "run"),
        )
        shutil.rmtree(keep, ignore_errors=True)
        try:
            shutil.copytree(det_dir, keep)
            print("[det] this RED's artifacts kept at %s" % keep)
        except OSError as e:
            print("[det] could not keep the red's artifacts: %s" % e)
        for old in sorted(glob.glob(os.path.join(_scratch(), "determinism.red-*")))[:-3]:
            shutil.rmtree(old, ignore_errors=True)
    return 0 if clean else 1


def ini_compose_selftest():
    """`python tools/ui_test.py --selftest` -- pure-Python checks of make_ini's composition
    (tooling:TL-SUITE-INIMERGE), no rig, no game. Exercises the two properties this item exists for:
    a fragment overriding an already-present section's key actually wins (ONE section, not a
    shadowed second one), and a shadowed override -- the shape every one of dead-ends
    G69/G96/G178/G181/G249/G266/G267 shares -- is CAUGHT rather than silently read back as the wrong
    value. Part of the same "checked without a rig" family as test_ui.det_standard_selftest.
    """
    fails = []

    def check(name, cond):
        print("   %-70s %s" % (name, "ok" if cond else "FAIL"))
        if not cond:
            fails.append(name)

    # ---- IniLayers itself: a later fragment overriding a key in an already-present section wins,
    # and the section is emitted ONCE (never a shadowed duplicate for GetPrivateProfile* to lose the
    # override in).
    L = IniLayers()
    L.add_fragment("[net]\nlockstep_step_ms=30\nrx_spin=0\n")
    L.add_fragment("[net]\nlockstep_step_ms=60\n")  # a later, higher-precedence layer
    out = L.render()
    check(
        "a fragment overriding a key in an already-present section wins, in ONE [net] block",
        ini_effective(out, "net", "lockstep_step_ms") == "60"
        and ini_effective(out, "net", "rx_spin") == "0"
        and out.lower().count("[net]") == 1,
    )

    # ---- make_ini end to end: --net-extra overrides NET_BLOCK's own compiled-in default and the
    # result round-trips (make_ini's own internal assert would already have raised if it did not --
    # this just confirms the VALUE, not merely the absence of a raise).
    global EXTRA_NET
    saved_net_extra = EXTRA_NET
    EXTRA_NET = "lockstep_step_ms=60"
    try:
        ini = make_ini("walk.txt", 1500, is_host=True, ident={"port": 6601})
    finally:
        EXTRA_NET = saved_net_extra
    check(
        "make_ini: --net-extra overrides NET_BLOCK's default and reads back",
        ini_effective(ini, "net", "lockstep_step_ms") == "60",
    )

    # ---- THE PLANTED SHADOWED OVERRIDE. Before this rewrite, make_ini's --extra-ini-host/-client
    # tail was CONCATENATED after the ini it had already built (`ini += "\n" + tail`), so a fragment
    # naming a section make_ini itself already emits became a shadowed, dead SECOND section --
    # dead-ends G69/G181/G266/G267's exact shape (tools/uiscripts/ini/u28_off.ini, a real committed
    # fragment, is written in it). IniLayers cannot produce that shape any more, so reconstruct it
    # directly and confirm the reader model make_ini's round-trip assert uses would have refused it
    # -- i.e. that the assert is a real net, not a no-op.
    planted = "[net]\nstart_slots=1\n\n[net]\nstart_slots=0\n"  # a later dup section: dead text
    requested_value = "0"  # what the shadowed (second) [net] fragment asked for
    got = ini_effective(planted, "net", "start_slots")
    check(
        "a planted shadowed override (the pre-fix concatenation shape) is CAUGHT, not silently 1",
        got != requested_value,
    )

    # ---- And the channel that shape came in through is refused outright now, before it can ever
    # reach a shadowed section (closes the gap --extra-ini-host/-client left open; --extra-ini
    # itself was already refused).
    raised = False
    try:
        _refuse_net_fragment("[net]\nstart_slots=0\n", "planted.ini", "--extra-ini-host")
    except SystemExit:
        raised = True
    check("--extra-ini-host carrying [net] is REFUSED before it can shadow anything", raised)

    ok = not fails
    print("ini_compose_selftest: %s" % ("PASS" if ok else ("FAIL: " + ", ".join(fails))))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "script", nargs="?", help="single-peer: a script under tools/uiscripts/ (or a path)"
    )
    ap.add_argument(
        "--host", help="multi-peer HOST: 'script' (local dev box) or 'ip:script' (a VM)"
    )
    ap.add_argument(
        "--client",
        action="append",
        default=[],
        metavar="IP:SCRIPT",
        help="multi-peer: a VM client 'ip:script' (repeatable)",
    )
    ap.add_argument(
        "--connect-ip",
        default=None,
        help="IP the clients connect to (written into their setup.dat); default = the host's IP if the "
        "host is a VM, else --host-ip",
    )
    ap.add_argument(
        "--host-ip",
        default=machine.HOST_IP,
        help="the local dev box's LAN IP (used when the host is local)",
    )
    ap.add_argument(
        "--update-baselines", action="store_true", help="(re)generate baselines instead of diffing"
    )
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="pure-Python ini composition checks (no rig, no game); see tooling:TL-SUITE-INIMERGE",
    )
    # Deterministic identity: pin each peer's setup.dat (player name / game name / server IP) before launch
    # so the captured text is machine-independent (not the machine's saved history). --no-pin disables it.
    ap.add_argument(
        "--launch-args",
        default="",
        help="extra tokens placed BEFORE --skip-intro on the game command line, e.g. "
        '"--tactical 11". For a scenario the menu cannot reach; empty for every normal walk.',
    )
    ap.add_argument(
        "--deploy-save",
        default="",
        help="copy save/<NAME>.sav from the polygon into each peer dir before launch -- what a "
        "--launch-args verb taking a save name needs, since a lane has no save dir of its own.",
    )
    ap.add_argument("--host-name", default="host", help="host player name pinned into setup.dat")
    ap.add_argument(
        "--client-name", default="client", help="client player name (client N -> name+N)"
    )
    ap.add_argument("--game-name", default="uitest", help="game name the host creates (pinned)")
    # mp:R2b -- a CLIENT lane that itself HOSTS a lobby (the 3-peer browser_two_rows scenario: the
    # runner's host and its first client each create a game, the second client browses both). Its
    # created game name comes from ITS setup.dat, which pin_setup left at the lane template's saved
    # value -- machine state on a frame the baseline pins -- so pin it like --client-name does.
    ap.add_argument(
        "--client-game-name",
        default=None,
        help="game name pinned into each CLIENT's setup.dat (client N -> name+N, as --client-name); "
        "default: not pinned (a client that never creates a game does not read it)",
    )
    ap.add_argument(
        "--client-dead-ip",
        default=None,
        help="S8(b): give each client an IP MRU of [this DEAD ip, <connect-ip live host>] so the 'Server "
        "address' field pre-fills to the dead IP (entry 0) and the live host is the selectable 2nd entry -- "
        "the dead->correct->join round-trip test",
    )
    ap.add_argument(
        "--no-client-ip",
        action="store_true",
        help="mp:R7a: pin the client's name/game but NO server address, so the client has no saved server "
        "-- which is what makes the FIRST browser probe the relay (R7's no-saved-address case). Used by "
        "relay_punch (first-browser join); direct_dial keeps its pinned IP so the first browser stays quiet.",
    )
    # mp:GS1(b) / LA10: the PROCESS-EXIT relaunch shape -- one lane's game process quits to desktop
    # (the real llm_wnd_on_destroy -> ExitProcess exit, not a harness kill) and a LATER lane's process
    # is a brand-new one, launched only once the first is actually gone. The ordinary client loop below
    # launches every --client back-to-back right after the host is ready, which cannot express "wait
    # for that OTHER peer's process to exit first" -- there is no protocol message for a process dying,
    # and script-level `signal`/`awaitsignal` (peer_signals) only ferries a marker between processes
    # that are BOTH still running to read each other's log. So this is a launch-time gate, checked with
    # peer_liveness (the same PID-probe D15 built to tell a crash from a hang), not a script directive.
    ap.add_argument(
        "--client-after-exit",
        action="append",
        default=[],
        metavar="CLIENTIDX:PEERIDX",
        help="Delay launching --client #CLIENTIDX (1-based, in --client order) until peer #PEERIDX "
        "(0=host, 1=first --client, 2=second, ...) has ACTUALLY EXITED -- its process gone (peer_liveness "
        "alive=False), not merely its script reaching COMPLETE. Repeatable. The process-exit relaunch "
        "shape (mp:GS1(b): a client leaves, quits to desktop, and a NEW process re-joins).",
    )
    # mp:GS1(b) -- the companion half of --client-after-exit's OTHER end: the peer that DOES the
    # quitting never reaches its script's own COMPLETE marker (ExitProcess ends it first, by design),
    # so the overall verdict's `results.get(key) == "COMPLETE"` check would fail this scenario
    # FOREVER even on a perfect run. Naming a client here accepts a CONFIRMED SELF-DRIVEN exit
    # (exit_witness finds the D15 `; EXIT ...` line) as an equally-valid terminal state -- a process
    # that merely died some OTHER way (a crash, or a harness kill) still fails, because exit_witness
    # only reads "SELF-DRIVEN" off a real llm_wnd_on_destroy/utils_abort witness line.
    ap.add_argument(
        "--client-expect-exit",
        action="append",
        default=[],
        type=int,
        metavar="CLIENTIDX",
        help="--client #CLIENTIDX (1-based) is EXPECTED to end by quitting the process for real, not "
        "by its script reaching COMPLETE. Its terminal PROCESS-GONE counts toward the overall PASS "
        "only when exit_witness confirms a SELF-DRIVEN exit. Repeatable.",
    )
    ap.add_argument(
        "--no-pin",
        action="store_true",
        help="do NOT pin setup.dat identity (use the machine's own)",
    )
    ap.add_argument("--host-dir", default=machine.POLYGON)
    ap.add_argument("--vm-user", default=machine.VM_USER)
    ap.add_argument("--ssh-key", default=machine.SSH_KEY)
    ap.add_argument("--vm-dir", default=machine.VM_DIR)
    # mp:D29 (D3). A VM peer's game directory PERSISTS between runs, and remote_launch pushes every
    # satellite unconditionally -- so a VM could never run configuration (1): libmh.dll was always
    # re-deployed, and a stale copy from an earlier run was never deleted either. This is the VM
    # twin of make_lane's --omit-satellite: the named file is NOT pushed and is DELETED from the peer,
    # and the launch aborts if it is still there afterwards. Local lanes ignore it (make_lane already
    # built them without the file); test_ui passes it only on the VM topology.
    ap.add_argument(
        "--omit-satellite",
        action="append",
        default=[],
        metavar="[host:|client:]DLL",
        help="VM peers: do NOT deploy this satellite and DELETE any copy on the peer (repeatable). "
        "A `host:` / `client:` prefix scopes it to that side -- the MIXED configuration (1) shape "
        "omits libmh.dll on ONE peer only (mp:D29)",
    )
    # DEFAULT since 2026-08-27 (I6b): peers run byte-for-byte RETAIL mh.exe and
    # get mh.dll from the msvfw32 proxy shim. The peer file keeps its name; only its bytes change,
    # and it is deployed per run, so switching modes back and forth is safe.
    ap.add_argument("--stock-exe", action="store_true", help="(default; kept for compatibility)")
    ap.add_argument(
        "--patched-exe",
        action="store_true",
        help="opt OUT: run peers on the import-patched mh.focus.exe (the pre-2026-08-27 mechanism)",
    )
    ap.add_argument(
        "--port", type=int, default=6501, help="host TCP port (the client-ready listen probe)"
    )
    ap.add_argument(
        "--timeout", type=int, default=90, help="wall-clock seconds to wait for the run"
    )
    ap.add_argument(
        "--timeout-frames",
        type=int,
        default=None,
        help="[uitest] per-step watchdog, in FRAMES. Defaults to 1500 with the blit, and to "
        "80000 headless -- see the note where it is resolved.",
    )
    # --determinism PROGRESS watchdog. Complements --timeout, which is a wall clock and so cannot tell a
    # wedged run from a slow one; these bound how long NOTHING may happen. 0 disables either half.
    ap.add_argument(
        "--stall-timeout",
        type=int,
        default=120,
        help="determinism runs: abort if the host's step count has not advanced for this many seconds "
        "(default 120; 0=off). Well clear of a legitimate barrier park, which rx_timeout_ms bounds at "
        "~10 s before the link drops.",
    )
    ap.add_argument(
        "--launch-timeout",
        type=int,
        default=300,
        help="determinism runs: abort if the host has not logged its FIRST step within this many "
        "seconds (default 300; 0=off). Separate from --stall-timeout because 'no steps yet' is normal "
        "while the peers walk the menus and rendezvous in the lobby.",
    )
    ap.add_argument(
        "--shim",
        help="run tools/net_shim.py between the peers, targeting this host (ip or ip:port). The runner "
        "owns its lifetime and points the clients at THIS box. Use for latency / link-death scenarios.",
    )
    ap.add_argument(
        "--shim-delay", type=float, default=0.0, help="shim ONE-WAY delay in ms (rtt = 2x)"
    )
    ap.add_argument("--shim-jitter", type=float, default=0.0, help="shim jitter in ms")
    ap.add_argument(
        "--shim-listen-port",
        type=int,
        help="port the shim ACCEPTS on (default: --port). Set it when the shim and the HOST share a "
        "box -- they cannot share a port, and the shim binds first, so the game loses (see "
        "shim_listen_port()). The peers must dial this port.",
    )
    ap.add_argument(
        "--shim-control-port",
        type=int,
        default=SHIM_CONTROL_PORT,
        help="the shim's localhost TCP control port (default %d). test_ui.py gives each local shim "
        "row its own, so shim rows can run concurrently." % SHIM_CONTROL_PORT,
    )
    ap.add_argument("--shim-timeline", help="shim timeline file (tools/uiscripts/shim/*.txt)")
    ap.add_argument(
        "--shim-trigger",
        action="append",
        default=[],
        help='EVIDENCE-BOUNDED shim action (repeatable): JSON {"cmd": "blackhole on", "when": '
        '[["host", "mh_net.log", "barrier #5 BEGIN"], ["c1", "mh_uidrive.log", "LOG: CLIENT"]]} -- '
        "sent once, when every condition's regex is found in that LOCAL peer's log. See "
        "parse_shim_triggers.",
    )
    ap.add_argument(
        "--determinism",
        action="store_true",
        help="UI-path determinism: drive peers into the game via the real UI (no force-entry), run --steps "
        "in-game with the [harness] logger, then mp_analyze.py -> ALL PAIRS IDENTICAL",
    )
    ap.add_argument(
        "--steps",
        type=int,
        default=800,
        help="--determinism: compare AT LEAST this many in-game steps (poll-bounded, may overshoot)",
    )
    ap.add_argument(
        "--det-exclude",
        action="append",
        default=[],
        metavar="PEER",
        help="--determinism: pull this peer's logs (host, client1, ...) but leave it OUT of the "
        "all-pairs hash compare -- for a peer that leaves the match on purpose (mp:U19b's quitter), "
        "whose short hash log is not a vacuous run. Repeatable.",
    )
    ap.add_argument(
        "--harness",
        action="store_true",
        help="single-peer runs (--script): arm the in-game [harness] state logger for --steps steps "
        "and keep mh_harness.log. This is the SINGLE-PLAYER determinism oracle's run mode (P0-SPDET) "
        "-- two sequential runs of one peer, promoted vs unpromoted, compared by mp_analyze.sp_compare. "
        "Pair it with --extra-ini tools/uiscripts/ini/ship_config.ini for the promoted arm, and "
        "with [harness] pin_wallclock=1 + region_hash_step=1 or the time-tick regions are uncomparable.",
    )
    ap.add_argument(
        "--tol", type=float, default=0.02, help="max fraction of differing pixels to PASS"
    )
    ap.add_argument(
        "--defang",
        type=int,
        default=0,
        help="[net] defang_overlay value. DEFAULT 0 = THE SHIP (mh_net.example.ini): the resync "
        "barrier's wait-screen frame runs and RESYNC_IN_PROGRESS clears. 1 NOPs that store -- the "
        "rig's historical default until TL-RIG-DEFANG, and the reason a 2-second barrier storm read "
        "as '0 mode-8 frames' for two months (G274); pass it only to reproduce that blindness.",
    )
    ap.add_argument(
        "--net-extra",
        default="",
        help="extra [net] lines, ';'-separated k=v (e.g. 'defang_xui=1') -- per-group overlay-patch knobs.",
    )
    ap.add_argument(
        "--net-extra-client",
        default="",
        help="mp:R7a -- extra [net] lines for the CLIENT peers ONLY (the client-only twin of "
        "--net-extra). Built for direct_dial_with_relay_set: `relay=` on the client, the host off "
        "the relay, so an untouched relay (peers=0) is the assertion.",
    )
    ap.add_argument(
        "--signal-touch",
        action="append",
        default=[],
        metavar="NAME=PATH",
        help="mp:R4b -- when any peer's script emits `signal NAME`, create PATH (once). The way a "
        "script reaches the caller that started this run: test_ui.py's relay_restart scenario "
        "restarts its relay process on the peers' `signal ingame`.",
    )
    ap.add_argument(
        "--extra-ini-host",
        help="ini fragment for the HOST peer ONLY -- the asymmetric twin of --extra-ini. Built for "
        "O3: original container on one peer, promoted on the other, refereed by the lockstep hash. "
        "A symmetric promoted run cannot referee itself.",
    )
    ap.add_argument(
        "--extra-ini-client",
        help="ini fragment for every NON-HOST peer -- the other half of the asymmetric pair. Since "
        "promotion became the SHIPPING DEFAULT (C8-f), this is the half that actually creates the "
        "asymmetry: a client with no [promote] section still promotes, so making the original the "
        "reference oracle takes an explicit `[promote] lockstep=0` here.",
    )
    ap.add_argument(
        "--extra-ini",
        action="append",
        default=[],
        metavar="FILE",
        help="path to a file whose contents are merged into each peer's mh_net.ini (whole extra "
        "sections, e.g. tools/uiscripts/ini/debug_overlay.ini to switch the debug overlay on for one "
        "test). REPEATABLE: pass it N times and all N fragments are merged, in order, through "
        "ini_merge_fragment -- it used to be a scalar option that silently kept only the LAST one, "
        "which on 2026-08-02 armed 2 of 4 shadow sites on a run whose arming set check_arming_set.py "
        "had already validated over all 4.",
    )
    ap.add_argument(
        "--pixdelta",
        type=int,
        default=40,
        help="per-pixel max-channel delta that counts as differing",
    )
    ap.add_argument(
        "--record",
        type=int,
        default=0,
        help="harness order_mode: 1 = RECORD the dispatched order stream to mh_orders.bin (+ the "
        "mh_clock.bin clock track) in each peer's run folder, so an interactive session can be "
        "replayed later without a human. 2 = REPLAY a recording placed next to the exe.",
    )
    ap.add_argument(
        "--pull-logs",
        metavar="DIR",
        help="after the run, copy every peer's logs (mh_net.log and siblings, the process directory "
        "plus each session directory appended in stamp order) into DIR/<peer key>/ -- the same pull "
        "the --determinism path already does. What a post-run checker needs when a peer is a VM: "
        "its logs otherwise never leave the rig, which is why test_ui's own post_check_peers can "
        "only read LOCAL lanes. mp:L1f.",
    )
    ap.add_argument(
        "--order-log",
        type=int,
        default=0,
        help="harness order_log: 1 = also dump ORDER_PENDING/ORDER_QUEUE contents as ';ord' lines, "
        "which makes the recording readable (what was ordered, and when).",
    )
    ap.add_argument(
        "--ship-pacing",
        action="store_true",
        help="omit the rig's pinned lockstep_step_ms/sim_step_ms so the DLL's SHIPPING defaults apply "
        "(100 ms lookahead + adaptive controller, 20 ms sim sub-step). Use for a determinism gate that "
        "validates what players actually run.",
    )
    # HEADLESS IS THE DEFAULT (2026-07-28). [video] no_present=1 cuts the DirectDraw blit inside
    # llm_gfx_present_flip; the frame is still composed in software, so captures are BYTE-IDENTICAL
    # (A/B-verified by SHA-256), the suite is 12/12 on both topologies, it costs no wall clock (81622
    # presents / 9.8 s parked vs 83497 / 9.7 s visible) and it steals no focus (foreground sampled 94x:
    # the game 0 times). --headless stays accepted so existing invocations keep working.
    ap.add_argument("--headless", action="store_true", help="(default; kept for compatibility)")
    ap.add_argument(
        "--visible",
        action="store_true",
        help="opt OUT of headless: restore the DirectDraw blit and show the window. Use when you want "
        "to WATCH a run, and for anything that measures pacing (see --force-headless).",
    )
    # DESKTOP ISOLATION IS THE DEFAULT (2026-08-02). The game runs on its own Windows desktop object,
    # so its window cannot appear on, take focus from, or read the cursor of yours -- whatever creates
    # or shows it. Unlike --headless this makes no claim about the mechanism, which is exactly why it
    # is the default: headless cannot keep the window unmapped and three hypotheses about who maps it
    # were each refuted. Free: suite 12/12 in 4.5 min, identical to the interactive-desktop figure.
    ap.add_argument(
        "--desktop",
        nargs="?",
        const=desktop.DEFAULT_DESKTOP,
        default=None,
        help="name of the isolated desktop (default: %s). Isolation is ON unless --no-desktop or "
        "--visible; pass this explicitly to isolate a --visible run anyway, or to use a second "
        "desktop name. Peers sharing a name share one desktop." % desktop.DEFAULT_DESKTOP,
    )
    ap.add_argument(
        "--no-desktop",
        action="store_true",
        help="opt OUT of desktop isolation: run the game on YOUR interactive desktop, where it can "
        "take focus and read your cursor. Needed only when something outside the harness must reach "
        "the window (a debugger attach by click, a screen recorder).",
    )
    ap.add_argument(
        "--force-headless",
        action="store_true",
        help="permit headless for a --determinism / --ship-pacing run, which is otherwise refused: no "
        "blit means no vsync wait (~60 fps -> ~8500), and that is exactly what mp_pacing_report.py and "
        "the adaptive lookahead controller measure.",
    )
    ap.add_argument(
        "--harness-extra-host",
        default="",
        help="extra [harness] lines, ';'-separated k=v, merged into the HOST peer's [harness] block "
        "ONLY. Asymmetric by design -- it is how one peer is perturbed so the gate has something to "
        "find (e.g. 'rng_perturb_slot=2;rng_perturb_step=13000').",
    )
    ap.add_argument(
        "--harness-extra",
        default="",
        help="extra [harness] lines, ';'-separated k=v, merged into EVERY peer's [harness] block. "
        "Symmetric, so unlike --harness-extra-host it cannot create the asymmetry a determinism run "
        "is measuring. P0-SPDET's single-player oracle rides here: "
        "'pin_wallclock=1;fixed_step=0;region_hash_step=1'.",
    )
    ap.add_argument(
        "--ai-probe",
        type=int,
        default=0,
        metavar="N",
        help="harness ai_probe_step: emit an '; AIPROBE' line every N steps carrying the AI master "
        "gate, the ai_players_tick loop bound, every player's ai_enabled, and the AI PRNG slot. Use on "
        "AI-active runs (D10) so 'the AI actually ran' is read off the log rather than assumed.",
    )
    ap.add_argument(
        "--dll",
        default="",
        metavar="PATH",
        help="deploy THIS mh.dll (and its sibling .pdb) into every peer instead of the Release "
        "build. For tools/coverage.py, which needs the UNOPTIMISED one: a Release /O2+LTCG binary "
        "reports inlined-away bodies as 0%% covered, indistinguishable from never executed. Without "
        "this the per-launch copy in local_launch silently undoes make_lane's own --dll.",
    )
    args = ap.parse_args()
    if args.selftest:
        return ini_compose_selftest()
    for spec in args.omit_satellite:
        role, _, sat = spec.rpartition(":")
        if role not in ("", "host", "client") or sat not in SATELLITES:
            ap.error(
                "--omit-satellite %s: want [host:|client:]DLL, DLL one of %s"
                % (spec, ", ".join(SATELLITES))
            )
    if args.dll:
        if not os.path.isfile(args.dll):
            sys.exit("--dll: no such file: %s" % args.dll)
        global DLL_OVERRIDE
        DLL_OVERRIDE = os.path.abspath(args.dll)
        print("[dll] deploying %s (overrides the Release build)" % DLL_OVERRIDE)
    # Same shape as make_lane's `args.headless = not args.visible`: the flag that TRAVELS is
    # the opt-out, and the effective mode is derived once, here.
    args.stock_exe = not args.patched_exe

    # mp:GS1(b) -- parsed once, here, into {client_idx(1-based): peer_idx(0-based)}. Validated eagerly
    # (a bad index is an authoring mistake in a [uitest] registry row, not a runtime condition) so a
    # typo aborts before any process launches rather than silently never gating anything.
    args.client_after_exit_map = {}
    for spec in args.client_after_exit:
        try:
            cidx_s, pidx_s = spec.split(":", 1)
            cidx, pidx = int(cidx_s), int(pidx_s)
        except ValueError:
            ap.error("--client-after-exit wants CLIENTIDX:PEERIDX (both integers), got %r" % spec)
        if cidx < 1:
            ap.error(
                "--client-after-exit: CLIENTIDX is 1-based (first --client is 1), got %d" % cidx
            )
        if pidx < 0 or pidx >= cidx:
            ap.error(
                "--client-after-exit %s: PEERIDX must be an EARLIER peer (0=host, < CLIENTIDX) -- "
                "a client cannot wait on a peer that has not launched yet" % spec
            )
        args.client_after_exit_map[cidx] = pidx

    global \
        DEFANG_OVERLAY, \
        EXTRA_NET, \
        CLIENT_NET_EXTRA, \
        RUN_TRANSPORT, \
        EXTRA_INI, \
        EXTRA_INI_HOST, \
        EXTRA_INI_CLIENT, \
        SHIP_PACING, \
        ORDER_MODE, \
        ORDER_LOG, \
        AI_PROBE_STEP, \
        HEADLESS, \
        DESKTOP
    global HARNESS_EXTRA_HOST, LAUNCH_ARGS, DEPLOY_SAVE
    LAUNCH_ARGS = args.launch_args
    DEPLOY_SAVE = args.deploy_save
    DEFANG_OVERLAY = args.defang
    SHIP_PACING = args.ship_pacing
    HEADLESS = resolve_headless(args, ap)
    # HOLD the desktop for the whole process, not for a `with` block: this runner spawns its peers
    # and then blocks for the entire scenario, and the desktop dies with its last handle. Released by
    # the OS at exit, which is exactly when the last peer is gone.
    # Resolved here, HELD LAZILY at the first local launch (see local_launch). A VM-topology run --
    # every --determinism run, and the suite's --no-local mode -- launches no game on this box, so
    # eagerly creating a desktop there would do nothing but print a banner claiming an isolation that
    # is not in play.
    DESKTOP = resolve_desktop(args)
    # THE FRAME WATCHDOG HAS TO FOLLOW THE BLIT. It is budgeted in FRAMES, and headless removes the
    # vsync wait -- ~60 fps becomes ~8500 -- so 1500 frames does not survive boot on this host
    # (test_ui.py has carried an 80000 floor for its own lanes since 2026-07-28 for exactly this
    # reason). Before the `global HEADLESS` fix on the line above, the default path never actually
    # went headless, so this mismatch could not fire; fixing one without the other turns every
    # default-polygon run into a TIMEOUT. Only the DEFAULT is scaled -- an explicit --timeout-frames
    # is an instruction and is left alone.
    if args.timeout_frames is None:
        args.timeout_frames = 80000 if HEADLESS else 1500
        if HEADLESS:
            print("[rig] headless: per-step frame watchdog raised to %d" % args.timeout_frames)
    AI_PROBE_STEP = args.ai_probe
    HARNESS_EXTRA_HOST = args.harness_extra_host
    global HARNESS_EXTRA
    HARNESS_EXTRA = args.harness_extra
    if HARNESS_EXTRA_HOST:
        print("[cfg] HOST-ONLY [harness] extras: %s" % HARNESS_EXTRA_HOST)
    ORDER_MODE = args.record
    ORDER_LOG = args.order_log
    EXTRA_NET = args.net_extra
    CLIENT_NET_EXTRA = args.net_extra_client
    if CLIENT_NET_EXTRA:
        print("[cfg] CLIENT-ONLY [net] extras: %s" % CLIENT_NET_EXTRA)
    for kv in args.signal_touch:
        if "=" not in kv:
            sys.exit("--signal-touch wants NAME=PATH, got %r" % kv)
        n, p = kv.split("=", 1)
        SIGNAL_TOUCH[n.strip()] = os.path.abspath(p.strip())
    RUN_TRANSPORT = resolve_transport(EXTRA_NET)
    if RUN_TRANSPORT != "udp":
        # Printed unconditionally for the non-default choice: every readiness line, every abort and
        # every verdict below is about THIS transport, and a run transcript that does not say which
        # one cannot be read afterwards. (udp is the default since 2026-09-20; tcp is the explicit one.)
        print(
            "[cfg] transport: %s -- mh.dll binds mh_net.dll, and the host-ready probe reads the "
            "TCP listener table" % RUN_TRANSPORT
        )
    for one in args.extra_ini:
        path = one
        if not os.path.isabs(path):
            path = os.path.join(REPO, path)
        with open(path, "r", encoding="utf-8") as fh:
            text = fh.read()
        # REFUSED, not merged: fragments are merged with EACH OTHER and then appended as a TAIL to an
        # ini that already opens with its own [net] block -- so a [net] section here becomes the
        # SECOND one in the file, and GetPrivateProfile* reads only the first. Every key in it is
        # silently inert.
        #
        # This is the third instance of one trap. The [video] merge below and the --net-extra
        # "first match wins" guard above are the other two, and both were found the same way: a run
        # that reported success about a configuration it never had. Measured here 2026-08-27 -- three
        # 2-peer determinism runs at 3000, 3000 and 2000 steps, an A/B of a fix knob, all reporting
        # ALL PAIRS IDENTICAL, and all three ran the DEFAULT config because the fragment's [net] was
        # dead text. The A and B arms were the same run.
        #
        # Refused rather than merged on purpose: [net] has a dedicated channel that writes into the
        # base section, so a fragment reaching for it is a mistake with an obvious right answer, and
        # silently doing what was meant would leave the next author with the same wrong mental model.
        _refuse_net_fragment(text, one, "--extra-ini")
        EXTRA_INI = ini_merge_fragment(EXTRA_INI, text)
        print("[cfg] ini fragment: %s" % one)
    if args.extra_ini_host:
        path = args.extra_ini_host
        if not os.path.isabs(path):
            path = os.path.join(REPO, path)
        with open(path, "r", encoding="utf-8") as fh:
            EXTRA_INI_HOST = fh.read()
        # TL-SUITE-INIMERGE: this refusal used to exist only for the general --extra-ini above, not
        # for its host/client twins -- so a [net] section here (tools/uiscripts/ini/u28_off.ini is a
        # real, if unregistered, example) was silently shadowed by the base [net] block and caught
        # only downstream, if at all, by make_ini's own round-trip assert with a far less specific
        # message. Same refusal, same reason, one channel closer to the mistake.
        _refuse_net_fragment(EXTRA_INI_HOST, args.extra_ini_host, "--extra-ini-host")
        print("[cfg] HOST-ONLY ini fragment: %s" % args.extra_ini_host)
    if args.extra_ini_client:
        path = args.extra_ini_client
        if not os.path.isabs(path):
            path = os.path.join(REPO, path)
        with open(path, "r", encoding="utf-8") as fh:
            EXTRA_INI_CLIENT = fh.read()
        _refuse_net_fragment(EXTRA_INI_CLIENT, args.extra_ini_client, "--extra-ini-client")
        print("[cfg] CLIENT-ONLY ini fragment: %s" % args.extra_ini_client)

    # PREFLIGHT: does the pinned view even fit the desktop? Refuse LOUDLY rather than let the run
    # reproduce G35 -- see view_exceeds_desktop for what that looks like (it looks like a sim hang).
    _view = pinned_view_size(EXTRA_INI + "\n" + EXTRA_INI_HOST + "\n" + EXTRA_INI_CLIENT)
    _desk = primary_desktop_size()
    if view_exceeds_desktop(_view, _desk):
        print(
            "[cfg] ABORT: this run pins a %dx%d view but the primary desktop is only %dx%d.\n"
            "      The game will NOT report a mode failure -- it launches, walks the menu, reaches\n"
            "      session_begin_multi, serves one [promote] time_tick call and then stops stepping,\n"
            "      so the script hangs on its next wait until the timeout and it reads as a sim bug\n"
            "      -- it reads as a sim bug. Raise the desktop to at least %dx%d, or drop the\n"
            "      --extra-ini display pin, and re-run." % (_view + _desk + _view)
        )
        return 2
    if _view and _desk:
        print("[cfg] display: pinned view %dx%d fits desktop %dx%d" % (_view + _desk))

    if args.determinism:
        return run_determinism(args)

    def resolve(name):
        return resolve_script(name)

    overall_ok = True

    if args.script and not args.host and not args.client:
        # ---- single-peer (local, no networking) ----
        # --extra-ini-host is meaningless here and, worse, USED TO BE SILENT: peer_launch below is
        # called without is_host, so make_ini skipped both the append AND the self-check that exists to
        # catch exactly this. The flag parsed, the "[cfg] HOST-ONLY" line printed, nothing reached the
        # ini, and the run passed. Same failure the self-check was written for, in the one mode it did
        # not cover. Refuse rather than quietly symmetrise: with one peer there is no asymmetry to
        # make, so --extra-ini is what the caller meant.
        if EXTRA_INI_HOST:
            raise SystemExit(
                "--extra-ini-host on a single-peer run: there is no second peer for the fragment to "
                "differ from, so it would silently do nothing. Use --extra-ini."
            )
        if EXTRA_INI_CLIENT:
            raise SystemExit(
                "--extra-ini-client on a single-peer run: there is no client peer to aim it at, and "
                "the lone peer launches as the HOST -- so it would silently do nothing. Use "
                "--extra-ini."
            )
        src = resolve(args.script)
        label = os.path.splitext(os.path.basename(src))[0]
        deadline = time.time() + args.timeout
        pin_setup(args, None, name=args.host_name, game=args.game_name)  # deterministic identity
        # P0-SPDET: --harness arms the in-game state logger on a SINGLE-PEER run, which is what the
        # single-player oracle needs (two sequential runs of one peer, promoted vs unpromoted). It is
        # an explicit flag rather than `--steps > 0` because --steps DEFAULTS to 800: keying off it
        # would arm the harness on every capture test in the suite, and a stop_step halts the sim
        # mid-walk -- the exact hazard local_launch's stale-ini removal is already there to prevent.
        run = peer_launch(args, None, src, args.timeout_frames, args.steps if args.harness else 0)
        status = None
        if args.harness and run:
            # A HARNESS run must outlive its script. The scripts end at `end` the moment the game is
            # entered, and the capture-suite wait below kills the process as soon as that marker
            # appears -- so a 400-step run died four seconds in, having logged nothing. Wait on the
            # SAME signal --determinism waits on (the logged step count) with the same stall
            # watchdog; a third wait loop is what run_determinism's own comment warns against.
            last_n, last_change = None, time.time()
            while time.time() < deadline:
                time.sleep(4)
                n, done = peer_harness_steps(args, None, run)
                if done or n >= args.steps:
                    status = "HARNESS-DONE(%s steps)" % n
                    break
                if n != last_n:
                    last_n, last_change = n, time.time()
                    continue
                quiet = time.time() - last_change
                # A DEAD peer is not worth waiting out the stall budget for. Checked at a quarter of
                # it rather than every poll: the liveness probe is a PowerShell round trip, and this
                # loop runs every 4 s for the whole run.
                if quiet > max(15, args.stall_timeout / 4) and not _crash_checked.get(run):
                    _crash_checked[run] = True
                    alive, lines = peer_liveness(run)
                    if lines or alive is False:
                        for ln in lines:
                            print(ln)
                        status = (
                            ("CRASHED at %s steps" % n)
                            if lines
                            else ("PROCESS-GONE at %s steps" % n)
                        )
                        break
                if stall_abort(n, quiet, args.launch_timeout, args.stall_timeout):
                    status = peer_verdict(run, "STALLED(at %s steps for %ds)" % (n, int(quiet)))
                    break
            print("[host] harness %s" % (status or "WALL-CLOCK TIMEOUT"))
            # The script's own verdict is still worth printing: a TIMEOUT there means the walk did
            # not finish, which is a different failure from the harness not stepping.
            print("[host] script %s" % (local_script_status(run) or "(no marker)"))
        else:
            while run and time.time() < deadline and not status:
                time.sleep(2)
                status = local_script_status(run)
            print("[host] script %s" % (status or "TIMED-OUT(no marker)"))
        local_kill()
        if not run:
            return 1
        if args.harness:
            # A harness run is judged on whether it STEPPED, not on frames: it is a determinism arm,
            # not a capture test, and comparing its incidental captures against a baseline would fail
            # it for reasons that have nothing to do with the sim.
            overall_ok = bool(status) and status.startswith("HARNESS-DONE")
        else:
            png_dir = pull_local_captures(run)
            overall_ok = collect_and_check(label, png_dir, args) and status == "COMPLETE"
    elif args.host:
        # ---- multi-peer. The host may be local OR a VM (`ip:script`). Launch the host, wait until it's
        #      LISTENING (ready for clients), point each client's setup.dat at the host + launch it, then
        #      wait for ALL peers to finish (a host that gates on `peers N` only COMPLETEs after the
        #      clients join, so we can't wait for host COMPLETE up front). Two-VM topology frees the dev box.
        host_ip, hspec, hdir = parse_peer(args.host)
        hsrc = resolve(hspec)
        connect_ip = args.connect_ip or host_ip or args.host_ip
        peers = [
            {
                "key": "host",
                "ip": host_ip,
                "dir": hdir,
                "src": hsrc,
                "label": os.path.splitext(os.path.basename(hsrc))[0],
            }
        ]
        # mp:GS1(b): keys seen so far, so a SECOND --client resolving to the same key (client_shares_lane
        # -- two --client entries pointed at the SAME lane=/ip, safe only because --client-after-exit
        # already ordered them) gets disambiguated rather than colliding. Colliding keys would make
        # `peers` (a list) and `pending` (a SET keyed by this string) disagree about how many peers
        # exist: the unified wait below, and the periodic liveness check, both index by `p["key"]", so
        # two peer dicts sharing one key take turns overwriting each other's `results` entry and can
        # retire `pending` on the WRONG one's status -- observed as a live, still-running client
        # reported "PROCESS IS GONE" (true of the peer it reused the key from) while the actual
        # peer's own COMPLETE never got the chance to register. Every OTHER call site keeps its
        # ordinary key unchanged; only a genuine collision is renumbered, and only from its 2nd use.
        _seen_client_keys = {}
        for ci, spec in enumerate(args.client):
            cip, cspec, cdir = parse_peer(spec)
            # A LOCAL client must name its own lane: peers sharing one folder would fight over
            # setup.dat / mh_net.ini / logs, which is a corrupted run rather than an error.
            if cip is None and not cdir:
                ap.error("a local --client must be 'lane=<name>:script' (it needs its own folder)")
            csrc = resolve(cspec)
            base_key = cip or os.path.basename(cdir)
            n = _seen_client_keys.get(base_key, 0) + 1
            _seen_client_keys[base_key] = n
            key = base_key if n == 1 else "%s#%d" % (base_key, n)
            peers.append(
                {
                    "key": key,
                    "ip": cip,
                    "dir": cdir,
                    "src": csrc,
                    "label": os.path.splitext(os.path.basename(csrc))[0],
                    "expect_exit": (ci + 1) in args.client_expect_exit,
                }
            )

        shim = shim_start(args)
        if args.shim and not shim:
            return 1
        if shim:  # peers must dial the shim, not the host
            # ...but only REPOINT them when the shim is on a different box than the peers. With local
            # lanes everyone is on 127.0.0.1 already, and the shim's own port (shim_listen_port) is what
            # separates it from the host -- swapping in the LAN ip there would just be a longer route to
            # the same socket.
            if host_ip is not None:
                connect_ip = local_lan_ip()
            print("[shim] clients will connect to %s:%d" % (connect_ip, shim_listen_port(args)))
            bad = shim_lane_port_mismatch(args, [p.get("dir") for p in peers[1:]])
            if bad:
                shim_stop(shim)
                sys.exit(bad)

        deadline = time.time() + args.timeout
        host = peers[0]
        print("[host %s] launching %s ..." % (host_ip or "local", os.path.basename(hsrc)))
        # pin the HOST identity (player name + created game name) so its lobby + the clients' browser rows
        # render deterministic text regardless of the machine's saved history.
        pin_setup(args, host_ip, name=args.host_name, game=args.game_name, pdir=host.get("dir"))
        # `is_host=True` -- mp:X1b, and it is a BUG FIX rather than a new capability. `--harness-extra-host`
        # is parsed, printed by test_ui's "[cfg] HOST-ONLY [harness] extras:" line and documented as the
        # asymmetric twin of --harness-extra, but this launcher (the multi-peer UI-SCRIPT path, as
        # distinct from --determinism's at the top of this file) never told peer_launch which peer it
        # was launching, so make_harness_ini's `is_host` was False for the host too and every host-only
        # key was silently dropped. Measured: `--harness-extra-host snapshot_at=120` produced a lane ini
        # with no snapshot_at in it while the runner printed the flag back. Inert for every scenario
        # that does not pass the flag (HARNESS_EXTRA_HOST is "" and both helpers return "").
        host["run"] = peer_launch(
            args, host_ip, hsrc, args.timeout_frames, is_host=True, pdir=host.get("dir")
        )
        if not host["run"]:
            peer_kill(args, host_ip, host.get("run"))
            shim_stop(shim)  # TL-RIG6: this abort path used to leak the shim's port to the next run
            return 1
        # The readiness gate answers ONE question: may the clients launch yet? With no clients there is
        # nothing to gate, and running it anyway is actively wrong -- the host only starts listening when
        # its walk reaches the lobby, which for a 23-step script is well past the 50 s ready budget. So a
        # single-peer run was killed mid-walk with "never became ready" (measured 2026-07-28 on a local
        # lane: the script log ended at step 11 of 23 with the game rendering happily).
        #
        # It survived this long because the probe port and the lane port only diverge locally: a lane
        # binds its own per-test port while test_ui's solo branch left ui_test on the 6501 default, so
        # the gate watched a port nothing in this run ever binds. Why the same gate does not kill solo
        # runs on the VM topology has NOT been established -- do not assume it is sound there either.
        if len(peers) < 2:
            print(
                "[host %s] single-peer run -- no readiness gate (no clients to launch)"
                % (host_ip or "local")
            )
        elif not _wait_host_ready(args, host_ip, host):
            # D15: the two ways this gate fails want OPPOSITE next steps, so do not print one
            # message for both. A dead process is not a budget problem, and telling the reader to
            # raise --timeout for it is how the 2026-08-06 sessions ended up investigating budgets
            # that were only ever 3-16% used.
            if host.get("dead"):
                print(
                    "[host] aborting: the host process is GONE, so no budget would have helped. "
                    "This is NOT a timeout -- do not raise --timeout for it. Check the lane's logs "
                    "and `python tools/crash_report.py --lane <lane dir>`."
                )
            else:
                print(
                    "[host] never became ready (no LISTENING on %d within %ds) -- aborting. If the "
                    "polls above printed ssh timeouts, suspect the PROBE, not the game: verify from "
                    "here with a TCP connect to <host>:%d before believing the host is down "
                    "(2026-07-27 -- see remote_listening). If instead the host was still WALKING, "
                    "raise --timeout: this gate now spends the whole run budget waiting."
                    % (args.port, args.timeout, args.port)
                )
            peer_kill(args, host_ip)
            shim_stop(shim)  # TL-RIG6: this abort path used to leak the shim's port to the next run
            return 1
        shim_arm(
            shim
        )  # mp:TL-SHIMUDP -- host confirmed ready; a manual-arm shim's clock starts here
        if len(peers) >= 2:
            print(
                "[host %s] ready -- launching clients (they connect to %s)"
                % (host_ip or "local", connect_ip)
            )
        for ci, p in enumerate(peers[1:]):
            client_idx = ci + 1  # 1-based, matches --client-after-exit's CLIENTIDX
            cname = args.client_name if ci == 0 else "%s%d" % (args.client_name, ci + 1)
            # mp:GS1(b) -- the PROCESS-EXIT relaunch shape: this client's launch is gated on an
            # EARLIER peer's process having actually exited (not merely finished its script), so a
            # fresh process joins where the old one left off, exactly like the field's crash/relaunch.
            # Checked here, in launch order, rather than folded into peer_launch: every other client
            # is unconditional and this keeps that path untouched.
            wait_pidx = args.client_after_exit_map.get(client_idx)
            if wait_pidx is not None:
                target = peers[wait_pidx]
                print(
                    "[client %s] waiting for peer #%d (%s) to EXIT (process gone) before launching ..."
                    % (p["key"], wait_pidx, target["key"])
                )
                exit_deadline = time.time() + args.timeout
                exited = False
                while time.time() < exit_deadline:
                    alive, _lines = peer_liveness(target.get("run"))
                    if alive is False:
                        exited = True
                        break
                    time.sleep(2)
                if not exited:
                    print(
                        "[client %s] ABORT: peer #%d (%s) never exited within %ds -- this is not a "
                        "process-exit relaunch if the earlier peer is still running"
                        % (p["key"], wait_pidx, target["key"], args.timeout)
                    )
                    for pp in peers:
                        if pp.get("run"):
                            peer_kill(args, pp.get("ip"), pp.get("run"))
                    shim_stop(shim)  # TL-RIG6: this abort path used to leak the shim's port too
                    return 1
                # exit_witness reads the D15 instrument, so the wait is provably on the REAL
                # llm_wnd_on_destroy -> ExitProcess path, not a process that merely died some other
                # way (a crash would also read alive=False here, and that is a different finding).
                for ln in exit_witness(target.get("run")):
                    print(ln)
                print(
                    "[client %s] peer #%d (%s) has exited -- launching a NEW process now"
                    % (p["key"], wait_pidx, target["key"])
                )
            # S8(b): a dead-IP round-trip test wants the field to pre-fill to a DEAD address (entry 0) with
            # the live host selectable in the MRU dropdown (entry 1); else pin the single live connect IP.
            # mp:R7a: --no-client-ip CLEARS the server-address MRU (an empty list, not None -- None would
            # keep the template's saved IP), so the client has no saved server. That is what makes the
            # FIRST browser probe the relay (R7's "no saved/typed address" case); a saved IP leaves it quiet.
            if args.no_client_ip:
                ip_val = []
            elif args.client_dead_ip:
                ip_val = [args.client_dead_ip, connect_ip]
            else:
                ip_val = connect_ip
            cgame = None
            if args.client_game_name:
                cgame = (
                    args.client_game_name if ci == 0 else "%s%d" % (args.client_game_name, ci + 1)
                )
            pin_setup(args, p["ip"], ip_val=ip_val, name=cname, game=cgame, pdir=p.get("dir"))
            print("[client %s] launching %s ..." % (p["key"], os.path.basename(p["src"])))
            p["run"] = peer_launch(args, p["ip"], p["src"], args.timeout_frames, pdir=p.get("dir"))

        # unified wait: every peer to reach COMPLETE / TIMEOUT
        results = {}
        pending = {p["key"] for p in peers if p.get("run")}
        delivered = set()  # (peer, signal) already ferried -- see pump_signals
        last_live = time.time()
        shim_triggers = parse_shim_triggers(args.shim_trigger) if shim else []
        trig_t0 = time.time()
        while pending and time.time() < deadline:
            time.sleep(3)
            if len(peers) > 1:
                pump_signals(args, peers, delivered)
            for p in peers:
                if p["key"] not in pending:
                    continue
                st = peer_status(args, p["ip"], p["run"])
                if st:
                    results[p["key"]] = st
                    pending.discard(p["key"])
                    print("[%s] script %s" % (p["key"], st))
            # D15: the same give-up _wait_host_ready got, for the peers that are already RUNNING.
            # A peer whose process has exited will never write a marker, so every second after that
            # is spent proving something already known -- and it is spent producing a timeout
            # message, which is the one reading this failure must not be given. Checked on a slow
            # cadence because peer_liveness costs a PowerShell round-trip per peer.
            if time.time() - last_live >= 10:
                last_live = time.time()
                for p in list(peers):
                    # VM peers included: peer_liveness answers for them too now (image-name query
                    # + their own Application log, pulled back). Leaving them out was the whole of
                    # the "crash detection on VM peers" gap.
                    if p["key"] not in pending:
                        continue
                    if report_dead_peer(p["key"], p["run"]):
                        results[p["key"]] = "PROCESS-GONE"
                        pending.discard(p["key"])
            pump_shim_triggers(shim, shim_triggers, peers, trig_t0)
        for k in pending:
            results[k] = "TIMED-OUT"
            print("[%s] script TIMED-OUT (no marker)" % k)
            run = next((p["run"] for p in peers if p["key"] == k), None)
            if run:
                report_dead_peer(k, run)

        # collect + diff every peer, then kill it
        for p in peers:
            if p.get("run"):
                # mp:L1f -- PULL THE LOGS TOO, when asked. Until this flag the log pull-back existed
                # only on the --determinism path, so a post-run CHECKER could read a LOCAL lane's
                # mh_net.log (it is on this box) but never a VM peer's -- which is exactly why
                # test_ui's own post_check_peers resolves lane directories and nothing else. A shape
                # whose topology has to be host-on-a-VM + client-on-a-VM + a local lane (the DET3
                # topology, because local 3-peer discovery cannot seat a second 127.0.0.1 client)
                # therefore had no way to get two of its three peers' evidence off the rig.
                if args.pull_logs:
                    dest = os.path.join(args.pull_logs, p["key"])
                    print(
                        "  [pull] %s logs -> %s"
                        % (p["key"], pull_peer_logs(args, p["ip"], p["run"], dest))
                    )
                png = peer_captures(args, p["ip"], p["run"])
                # mp:GS1(b) -- a peer named by --client-expect-exit is DESIGNED to end via a real
                # process exit rather than its script's own COMPLETE marker (ExitProcess ends it
                # first); accept that terminal state ONLY when exit_witness confirms it was
                # SELF-DRIVEN (the D15 `; EXIT ...` line), so a crash or a harness kill still fails.
                # bool(): a TIMED-OUT peer without expect_exit made this `True and None` -> None,
                # and `overall_ok &= None` raised TypeError HERE -- before peer_kill ran for the
                # remaining peers, so every timed-out --update-baselines run left its games alive
                # holding their lane mutexes (mp:R2b, 2026-09-22).
                status_ok = bool(
                    results.get(p["key"]) == "COMPLETE"
                    or (
                        p.get("expect_exit")
                        and results.get(p["key"]) == "PROCESS-GONE"
                        and any("SELF-DRIVEN" in ln for ln in exit_witness(p["run"]))
                    )
                )
                overall_ok &= collect_and_check(p["label"], png, args) and status_ok
            else:
                print("  [%s] no run dir -- launch failed" % p["key"])
                overall_ok = False
            peer_kill(args, p["ip"], p.get("run"))
        shim_stop(shim)
    else:
        ap.error("give a single script, or --host [ip:]script with --client ip:script")

    print("\nui_test: %s" % ("PASS" if overall_ok else "FAIL"))
    return 0 if overall_ok else 1


if __name__ == "__main__":
    sys.exit(main())
