#!/usr/bin/env python3
# tools/mp_run.py -- one-command N-peer MP test runner (Phase 5b of the MP instrumentation work).
#
# N1 (2026-07-22): generalized from host+1VM to host + K clients (--clients ip1,ip2,...). N = K+1; each
# client gets a distinct [net] player_id (1..N-1), the host is player 0, and mp_players=N drives the
# N-player force-entry (launch.cpp). For N>2 a >=N-start .mpm map (default 'Cold War.mpm') is deployed to
# every peer's Maps\. mp_analyze then does an all-pairwise region-hash diff. 2-peer runs are unchanged
# (default --clients = the single VM, TUTORIAL.MP).
#
# Orchestrates a HOST(this dev box) + K VM clients lockstep run end to end: build the run exe
# (no-CD + focus patches, see build_focus/build_no_cd -- so no peer needs the game disc mounted and
# alt-tabbing / interacting with either window can't pause-stall the lockstep -- Phase 5a), deploy
# it + the current mh.dll + the inis to both peers, launch the host GUI + the VM GUI (interactive
# scheduled task), wait for the run to finish, kill both, pull the VM's run folder, and run
# tools/mp_analyze.py on the host+client pair. Codifies the manual runbook (2026-07-10/11 runs).
#
# Uses the RUN-WITHOUT-FOCUS build on BOTH peers -- a plain mh.mp.exe pauses on focus loss, so a single
# alt-tab freezes the whole lockstep (learned the hard way 2026-07-11). A single-box variant is NOT
# possible: two full DirectDraw instances can't both grab the display on one machine (the 2nd hangs at
# display init before the menu) -- that needs the Phase 5c headless spike.
#
# Usage:
#   python tools/mp_run.py [--steps 800] [--step-ms 300] [--timeout 360]
#       [--host-ip <host>] [--host-dir <polygon>]   (defaults from tools/machine_config.py; EN)
#       [--vm-ip <vm>] [--vm-user <user>] [--vm-dir <vm-dir>]
#       [--ssh-key <key>] [--no-deploy]
#   (all machine-specific defaults live in tools/machine_config.py -- override there, not here)
#
# Restores the host's mh_net.ini afterward (ONE file since fork F2G). Assumes the VM is set up per the runbook
# (SSH key, compat shim, clean-VA mh.mp.exe present to re-patch from). A mounted 'Mh' CD is NO LONGER
# required on any peer since the no-CD stage landed (2026-07-25) -- a disc, if present, still works.

import argparse, os, sys, time, subprocess, glob, shutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # importable from any cwd
import gen_no_cd_manifest  # noqa: E402  bakes the per-layout no-CD manifest (the disc-check RE)
import desktop  # noqa: E402  the raw CreateProcessW launch that honours lpDesktop
import machine_config as machine  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DLL = os.path.join(REPO, "src", "mh_dll", "Release", "mh.dll")
# Satellite DLLs mh.dll loads beside itself (fork F4B; the spine joined at F4D). DERIVED from
# make_lane.DEFAULT_SATELLITES, not repeated -- a peer needs every one of them or it runs a
# different configuration, and the missing-libmh case in particular still boots and plays.
import make_lane  # noqa: E402  the one place the satellite set is written

SATELLITES = list(make_lane.DEFAULT_SATELLITES)
PATCHER = os.path.join(REPO, "src", "patcher")
MHPATCH = os.path.join(PATCHER, "mhpatch.py")
# Focus patch is build-specific (its guard site is a code VA). Try EN then RU; the wrong one GUARD-FAILs
# (harmless -- its per-patch expect bytes don't match) and we fall through to the other. EN is the DEFAULT
# host-dir now (machine.POLYGON, EN), so EN is tried first; the RU fallback keeps a RU --host-dir run working.
FOCUS_MANIFESTS = [
    os.path.join(PATCHER, "run_without_focus_EN.mh.patch.json"),  # EN /eng/mh.exe (default)
    os.path.join(
        PATCHER, "run_without_focus.mh.patch.json"
    ),  # RU /mh.exe (frozen reference fallback)
]
# Everything a post-mortem may want, pulled from each peer's run folder. mh_frametime.log joined the
# list when it started shipping ON (2026-07-25): it is the per-PRESENT trace, i.e. the only record of
# what the player actually FELT, and it was being written on the peer and then left behind.
LOGNAMES = [
    "mh_harness.log",
    "mh_lockstep.log",
    "mh_net.log",
    "mh_launch.log",
    "mh_temporal.log",
    "mh_frametime.log",
]

# [trace] temporal=1 block (the MP latency notes confirmatory per-EVENT trace). VAs authored EN-canonical
# (the DLL is EN-only since the 2026-07-22 mh_lib refactor):
# frame / lockstep_pump / commit_horizon / sim_tick / send_lockstep_extend. The DLL routes [trace] funcs=
# through mh_port_code(), so these translate RU->EN automatically -- the SAME list works on the EN primary
# and the frozen RU build. time_tick+sim_step+present emit from their own (already build-agnostic) detours.
TRACE_TEMPORAL = (
    "\n[trace]\ntemporal=1\nfuncs=0x0043ecfa,0x0049c088,0x0049c189,0x0043f3eb,0x0049d33b\n"
)

# `enable=1` LEADS THE BLOCK (fork F2G, ruling Q6). The harness used to arm off the mere
# EXISTENCE of mh_harness.ini -- there was no enable key at all -- and D12 merged that file into
# mh_net.ini, where presence-of-a-section would have been the same fragility one layer in. Every
# writer of this block therefore states the arm explicitly, in the block itself, so "this run is
# instrumented" and the config that instruments it cannot be written apart. A stale mh_harness.ini
# beside the exe is REFUSED by the DLL now, not ignored.
HARNESS = (
    "[harness]\nenable=1\nseed_step=0\nseed_mode=2\nstop_step=%d\nexit_on_stop=0\n"
    "fixed_step=0\npin_fpu=1\nregion_hash_step=1\norder_mode=%d\norder_log=%d\n"
)

# D6 (2026-07-27): the synthetic moving-unit workload, ON BY DEFAULT.
# A determinism run over an IDLE world compares a world in which nothing happens, so a transient
# divergence re-converges for free and a CASCADING desync cannot be seen.
# This puts every peer's mothership in motion for the whole run.
#
# THE SEED IS DRAWN HERE, ONCE PER RUN, WITH os.urandom -- NOT from the game. A destination derived
# from the game's own PRNG would be identical on every peer by construction and would never exercise
# the wire. The same value goes into EVERY peer's ini, so the peers agree while the value stays fresh
# per run; it is printed and written into the run folder so a failing run can be pinned and replayed.
SYNTH = "synth_move=%d\nsynth_seed=%d\nsynth_at=%d\nsynth_every=%d\n"


def synth_seed():
    """A fresh 31-bit seed per run (GetPrivateProfileIntA is signed, so stay under 2^31)."""
    return int.from_bytes(os.urandom(4), "little") & 0x7FFFFFFF


def harness_extra(args):
    """Extra [harness] lines for EVERY peer (--harness-extra)."""
    return "".join(kv.strip() + "\n" for kv in (args.harness_extra or "").split(";") if kv.strip())


def net_ini(args, role, host, pid, n, mapname):
    # N1: player_id (own slot, 0=host / 1..N-1=clients), mp_players (N), mp_map drive the N-player
    # force-entry (launch.cpp). peers = expected connections (host: N-1 clients, client: 1 host).
    # host_assign=1 (all peers) makes the HOST assign each joiner its id over the wire (WELCOME) instead
    # of trusting the declared player_id -- the hand-clicked N-player path (tested via --lobby --host-assign).
    peers = (n - 1) if role == "host" else 1
    s = (
        "[net]\nrole=%s\nhost=%s\nport=6501\npeers=%d\nplayer_id=%d\nmp_players=%d\nmp_map=%s\n"
        "host_assign=%d\nlog=1\n"
        "lockstep_step_ms=%s\nlockstep_step_eps_ms=%s\nsim_step_ms=%s\nrx_spin=%d\n"
        "horizon_heartbeat_ms=%d\ndefang_overlay=%d\noverlay_gate=%d\nresync_wait_fix=%d\nlog_gamemode=%d\ngame_speed_pct=%d\n"
        "eager_advertise=%d\nhires_clock=%d\nqpc_clock=%d\nlockstep_log=1\n"
        % (
            role,
            host,
            peers,
            pid,
            n,
            mapname,
            args.host_assign,
            args.step_ms,
            args.step_eps_ms,
            args.sim_step_ms,
            args.rx_spin,
            args.heartbeat_ms,
            args.defang,
            args.overlay_gate,
            args.resync_fix,
            args.log_gamemode,
            args.game_speed,
            args.eager,
            args.hires,
            args.qpc,
        )
    )
    s += "hold_start=%d\n" % (
        1 if args.drop_test else 0
    )  # U14 drop test: dwell in the lobby (no auto-enter)
    # Arbitrary extra [net] lines (';'-separated k=v), e.g. the per-group overlay-patch knobs
    # defang_tt_wait/defang_tt_sync/defang_xui/defang_dismiss -- for isolating the freeze cause.
    for kv in (args.net_extra or "").split(";"):
        kv = kv.strip()
        if kv:
            s += kv + "\n"
    return s + (TRACE_TEMPORAL if args.temporal else "")


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def ssh(key, user, ip, remote, timeout=25):
    return sh(
        [
            "ssh",
            "-o",
            "ConnectTimeout=8",
            "-o",
            "StrictHostKeyChecking=no",
            "-i",
            key,
            "%s@%s" % (user, ip),
            remote,
        ],
        timeout=timeout,
    )


# OpenSSH >= 10 prints a three-line "not using a post-quantum key exchange algorithm" banner on
# STDERR for every ssh/scp to a server that lacks PQ kex. It is ~205 chars, so it does not merely
# clutter an error message -- it CONSUMES one: the excerpt below used to be [:160] of raw stderr,
# which meant a failing scp reported the banner and truncated away the actual cause ("No such file
# or directory"). Strip OpenSSH's "** " banner lines so the excerpt carries the real error. Measured
# 2026-08-27, on the determinism run's optional-artifact pulls.
def _ssh_err(r):
    text = r.stderr or r.stdout or ""
    keep = [ln for ln in text.splitlines() if ln.strip() and not ln.startswith("** ")]
    return " / ".join(keep).strip()[:200]


def scp(key, src, dst, timeout=60, quiet=False):
    """Copy via scp. `quiet` suppresses the failure line for OPTIONAL artifacts -- files that exist
    only under some run shapes (ui_test.OPTIONAL_ARTIFACTS), where "missing" is the normal case and a
    loud FAILED reads as a broken rig. The caller still gets the CompletedProcess and can check it."""

    # scp reads a local Windows path like "C:\dir\file" as remote host "C" (the drive-letter colon).
    # Forward-slashing LOCAL args (those without "@") avoids that; remote "user@ip:path" args are left
    # alone. Verified: "C:/dir/file" transfers fine, "C:\dir\file" fails.
    def norm(p):
        return p if "@" in p else p.replace("\\", "/")

    r = sh(
        [
            "scp",
            "-o",
            "ConnectTimeout=8",
            "-o",
            "StrictHostKeyChecking=no",
            "-i",
            key,
            norm(src),
            norm(dst),
        ],
        timeout=timeout,
    )
    if r.returncode != 0 and not quiet:
        print("    scp FAILED (%s -> %s): %s" % (src, dst, _ssh_err(r)))
    return r


def ps(script):
    return sh(["powershell", "-NoProfile", "-Command", script])


def newest_run(logs_dir, role):
    c = [d for d in glob.glob(os.path.join(logs_dir, "*_" + role)) if os.path.isdir(d)]
    c.sort(key=os.path.getmtime, reverse=True)
    return c[0] if c else None


def steps_done(run_dir):
    if not run_dir:
        return -1, False
    log = os.path.join(run_dir, "mh_harness.log")
    if not os.path.isfile(log):
        return -1, False
    n = 0
    done = False
    with open(log, errors="replace") as f:
        for ln in f:
            if ln[:1].isdigit():
                n += 1
            elif ln.startswith("; per-region breakdown"):
                done = True
    return n, done


def build_no_cd(src_exe, out_exe):
    # Stage 1 of the run exe: make the game runnable with NO game CD in any drive (the disc-check RE).
    # Peers otherwise need an 'MH'-labelled disc mounted or they park on a blocking insert-CD modal
    # before the lockstep ever starts -- which on a headless/VM peer just looks like a dead run.
    # "Fallback only": a real disc still takes the stock path (CD audio included); only the
    # scan-exhausted exit diverts to a cave pointing G_CD_DATA_PATH at the exe's own directory.
    #
    # The manifest is BAKED PER RUN rather than committed: it appends a section, and mhpatch requires
    # the cave VA to equal the input's next-free VA, so one committed file cannot be valid both on a
    # pristine exe and after net_load/caphike have appended their own sections. gen_no_cd_manifest
    # auto-detects RU vs EN from the site bytes, so this works on both --host-dir builds.
    # Non-fatal by design: if it can't be baked or applied, the run continues WITH the CD requirement
    # (loud warning) rather than failing a test run outright.
    man = out_exe + ".nocd.json"
    try:
        info = gen_no_cd_manifest.bake(src_exe, man, pin=False)
    except Exception as e:
        print("    WARN: no-CD bake skipped (%s) -- the run exe will still require the disc" % e)
        return src_exe
    try:
        r = sh([sys.executable, MHPATCH, "apply", src_exe, man, out_exe])
        out = (r.stdout or "") + (r.stderr or "")
        if (
            r.returncode != 0
            or "OK" not in out
            or any(t in out for t in ("GUARD-FAIL", "SKIP ", "UNRESOLVED", "checksum mismatch"))
        ):
            print("    WARN: no-CD patch did not apply -- disc still required:\n%s" % out.strip())
            return src_exe
        print("    no-CD patch: %s build, cave @ %#x" % (info["build"], info["cave_va"]))
        return out_exe
    finally:
        if os.path.exists(man):
            os.remove(man)


def build_focus(src_exe, out_exe):
    # mhpatch reports a bad guard as "GUARD-FAIL" (and still writes an UNPATCHED copy, exit 0) -- so a
    # wrong-build manifest would silently produce a focus exe that stalls on focus loss. Require an
    # all-OK report; try each candidate manifest and use the one whose guard matches this exe's build.
    # A GUARD-FAIL means "wrong build for this manifest" -> move on. A hard crash (rc!=0, e.g. a
    # transient lief parse-None right after a force-killed DirectDraw game held the file) -> RETRY the
    # same manifest a few times before giving up on it.
    if not os.path.isfile(src_exe):
        raise RuntimeError("focus source exe not found: %r (check --host-dir quoting)" % src_exe)
    # chain the no-CD stage first; its output (or src_exe, if it was skipped) feeds the focus patch
    nocd_tmp = out_exe + ".nocd.tmp"
    src_exe = build_no_cd(src_exe, nocd_tmp)
    last = ""
    try:
        for man in FOCUS_MANIFESTS:
            for attempt in range(4):
                r = sh([sys.executable, MHPATCH, "apply", src_exe, man, out_exe])
                out = (r.stdout or "") + (r.stderr or "")
                guard_fail = any(
                    t in out for t in ("GUARD-FAIL", "SKIP ", "UNRESOLVED", "checksum mismatch")
                )
                if r.returncode == 0 and "OK" in out and not guard_fail:
                    print("    focus manifest: %s" % os.path.basename(man))
                    return
                last = out
                if guard_fail:
                    break  # deterministic wrong-build -> next manifest, no retry
                time.sleep(2)  # transient (crash/None) -> retry same manifest
        raise RuntimeError("focus patch failed for all manifests (last):\n%s" % last)
    finally:
        if os.path.exists(nocd_tmp):
            os.remove(nocd_tmp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--steps", type=int, default=800)
    ap.add_argument(
        "--step-ms", type=float, default=30
    )  # lookahead (ms); 30 = the low-latency sweet spot on this LAN (2026-07-22, ~1.0x/0.4% stall, 3x lower input lag than 101). float: fractional ms allowed
    ap.add_argument(
        "--sim-step-ms", type=float, default=10
    )  # strategic sim sub-step interval; 10 = ~100Hz smooth unit motion (default 2026-07-22); 0 = game default 100ms/10Hz; independent of the network horizon
    ap.add_argument(
        "--step-eps-ms", type=float, default=0.01
    )  # auto-nudge the lookahead off the 10ms clock grid (anti-aliasing); 0 = exact grid value (ep-sweep)
    ap.add_argument(
        "--rx-spin", type=int, default=0
    )  # (b) Step 2: off-frame RX drain -- busy-poll the pump+commit during a horizon stall instead of yielding a frame
    ap.add_argument(
        "--heartbeat-ms",
        type=int,
        default=50,
        help="horizon-heartbeat cadence (perf-decouple); 0 = off / retail render-coupled",
    )
    ap.add_argument(
        "--defang",
        type=int,
        default=1,
        help="suppress the freeze overlays (mode-3 + mode-8 paths); pairs with heartbeat, on by default",
    )
    ap.add_argument(
        "--overlay-gate",
        type=int,
        default=0,
        help="root-cause fix: redirect the ungated wait-overlay call through a SYNC_RETRY_COUNTDOWN<0x38 "
        "gate so the modal shows ONLY on genuine silence (not every at-horizon frame). Test with --defang 0 "
        "to check whether the gate alone keeps lockstep alive (replaces the wait-overlay half of the defang).",
    )
    ap.add_argument(
        "--resync-fix",
        type=int,
        default=1,
        help="net_resync_wait_fix (DEFAULT ON): neuter llm_net_lockstep_sync_delay_stub's uninitialized-stack "
        "return (0x49c044 8B45D8 -> 31C090 = return 0) so the resync busy-wait is instant instead of a garbage-"
        "length spin. Fixes the known racy MP permanent-hang (mh_hang.dmp 2026-07-10 / the go-live collapse). "
        "Set 0 to A/B the hang.",
    )
    ap.add_argument(
        "--log-gamemode",
        type=int,
        default=0,
        help="diagnostic: DR0 write-log of _G_LLM_GAME_MODE transitions -> mh_gamemode.log",
    )
    ap.add_argument(
        "--net-extra",
        default="",
        help="extra [net] lines, ';'-separated k=v -- e.g. 'defang_xui=1;defang_tt_wait=2'. Sets the "
        "per-group overlay-patch knobs to isolate the freeze cause (see net_lockstep install_overlay_patches).",
    )
    ap.add_argument(
        "--harness-extra",
        default="",
        help="extra [harness] lines, ';'-separated k=v, merged into EVERY peer's [harness] block "
        "(e.g. 'mask_ctrl_group=0' to run the unmasked arm of D2's A/B).",
    )
    ap.add_argument(
        "--harness-extra-host",
        default="",
        help="extra [harness] lines, ';'-separated k=v, merged into the HOST's [harness] block ONLY. "
        "Asymmetric by design: it is how you corrupt one peer to prove the gate can go RED, e.g. "
        "'rng_perturb_slot=2;rng_perturb_step=400' (D3's AI-PRNG negative test).",
    )
    ap.add_argument(
        "--game-speed",
        type=int,
        default=0,
        help="pin game_speed percent on both peers (100=normal, 150=1.5x); 0=off. Movement-feel experiment",
    )
    ap.add_argument(
        "--eager",
        type=int,
        default=1,
        help="eager_advertise: advertise+commit the post-step horizon in the present hook (perf-decouple "
        "diagnostic; determinism-safe but NO rate effect -- committed is never binding). Both peers. 0=off",
    )
    ap.add_argument(
        "--hires",
        type=int,
        default=1,
        help="hires_clock: timeBeginPeriod(1) (finer Sleep/scheduler). NOTE: does NOT affect GetTickCount on "
        "modern Windows -> no sim-rate effect; use --qpc for the clock fix. 0=off",
    )
    ap.add_argument(
        "--qpc",
        type=int,
        default=1,
        help="qpc_clock: redirect the game's GetTickCount (IAT slot) to a QPC-derived ms count -> ~1 ms game-"
        "clock quantum -> shrinks the per-step discard (~0.92x -> ~1.0x at step=100). Determinism-safe. Both peers. 0=off",
    )
    ap.add_argument(
        "--open-link",
        action="store_true",
        help="deploy mh_key.txt='open' so the transport runs UNauthenticated + unencrypted (the "
        "pre-2026-07-25 wire). For A/B-ing the crypto's cost, or talking to an older DLL. Default is "
        "the shipped secure path with a pinned rig key.",
    )
    ap.add_argument(
        "--record",
        type=int,
        default=0,
        help="order_mode: 1 = record the order stream (mh_orders.bin) to bank a replay; 0 = off",
    )
    ap.add_argument(
        "--order-log",
        type=int,
        default=0,
        help="1 = dump ORDER_PENDING/ORDER_QUEUE contents as ;ord lines in mh_harness.log (settles the order_pending region)",
    )
    ap.add_argument(
        "--temporal",
        type=int,
        default=1,
        help="per-EVENT temporal trace -> mh_temporal.log (QPC-stamped; the lockstep-latency "
        "confirmatory trace). Adds a [trace] temporal=1 block to both peers. 0=off",
    )
    ap.add_argument("--timeout", type=int, default=360)
    ap.add_argument("--host-ip", default=machine.HOST_IP)
    ap.add_argument(
        "--host-dir", default=machine.POLYGON
    )  # EN is the default target (2026-07-16); RU = machine.RU_POLYGON
    ap.add_argument(
        "--clients",
        default=machine.RIG_PEER_A,
        help="N1: comma-separated client IPs; each gets player_id 1..N-1, N=len+1. Default = the "
        "single Hyper-V VM (2-player). 3-peer test: --clients <peerA>,<peerB>",
    )
    ap.add_argument(
        "--synth-move",
        type=int,
        default=1,
        help="D6: synthetic moving-unit workload (each peer orders its own mothership to a per-run "
        "RANDOM destination). DEFAULT ON -- a determinism run over an idle world cannot see a "
        "cascading desync. 0 disables it (say so when reporting the result).",
    )
    ap.add_argument(
        "--synth-seed",
        type=int,
        default=0,
        help="D6: pin the workload seed to reproduce a specific failing run (0 = fresh per run)",
    )
    ap.add_argument(
        "--desktop",
        nargs="?",
        const=desktop.DEFAULT_DESKTOP,
        default=desktop.DEFAULT_DESKTOP,
        metavar="NAME",
        help="run the LOCAL host game on an isolated Windows desktop (default: %s) so its window "
        "cannot take focus from, or be disturbed by, whoever is using this machine. The host peer "
        "is this box (machine.HOST_IP), so without this a stray click or keypress lands in the game "
        "and silently changes the run. Pass --no-desktop to use the interactive desktop."
        % desktop.DEFAULT_DESKTOP,
    )
    ap.add_argument(
        "--no-desktop",
        dest="desktop",
        action="store_const",
        const="",
        help="launch the host on the INTERACTIVE desktop (visible, but your input reaches the game)",
    )
    ap.add_argument("--synth-at", type=int, default=60, help="D6: first order step")
    ap.add_argument(
        "--synth-every",
        type=int,
        default=1,
        help="D6: re-issue cadence in steps (1 = EVERY step, which keeps units[] changing "
        "continuously so a divergence has somewhere to go; 0 = issue once)",
    )
    ap.add_argument("--vm-user", default=machine.VM_USER)
    ap.add_argument("--vm-dir", default=machine.VM_DIR)
    ap.add_argument(
        "--ssh-key", default=machine.SSH_KEY
    )  # LIVE key for the Hyper-V peers (clones share it)
    ap.add_argument(
        "--map",
        default="",
        help="N1: force-entry map on EVERY peer (must have >=N starts). Default auto: TUTORIAL.MP "
        "for 2 players, 'Cold War.mpm' (Maps\\, 128x128/4-start) for >2. A .mpm with spaces is "
        "deployed space-free (scp remote paths choke on spaces).",
    )
    ap.add_argument(
        "--host-assign",
        type=int,
        default=0,
        help="N1: 1 = the HOST assigns each joiner its player id over the wire (WELCOME); every "
        "client is deployed with the SAME player_id so only the host assignment distinguishes "
        "them (the hand-clicked N-player path). Pairs with --lobby. 0 = declared-id (default).",
    )
    ap.add_argument("--no-deploy", action="store_true")
    ap.add_argument(
        "--ingame-kill",
        default="",
        help="U17: kill this client IP mid-game (once the host passes --ingame-kill-at steps) to test the "
        "fast-drop -> survivors must keep stepping and stay all-pairwise IDENTICAL. Use with >=3 peers so the "
        "game continues (a 2-peer kill ends the session).",
    )
    ap.add_argument(
        "--ingame-kill-at", type=int, default=1200, help="host step at which --ingame-kill fires"
    )
    ap.add_argument(
        "--lobby",
        action="store_true",
        help="Phase 2 (Workstream U): drive BOTH peers through the REAL lobby "
        "(--mp-host-lobby / --mp-join-lobby, cursor-free) instead of the force-entry "
        "bypass (--mp-host / --mp-join). Host auto-starts once the client's slot syncs.",
    )
    ap.add_argument(
        "--drop-test",
        action="store_true",
        help="U14 DROP path: set [net] hold_start=1 so the host DWELLS in the lobby (no auto-enter), "
        "wait for the client to slot, kill ONLY the client (transport drop, no LEAVE), then verify the "
        "host emits a 'U14 ... DROPPED' delta in mh_launch.log (implies --lobby). No determinism analyze.",
    )
    args = ap.parse_args()
    # D6: draw ONE seed per run and give it to every peer, so all peers pick the same
    # destination (the test must not desync itself) while the value stays fresh per run.
    if args.synth_move and not args.synth_seed:
        args.synth_seed = synth_seed()
    if args.synth_move:
        print(
            "[D6] moving-unit workload ON: seed=%d at=%d every=%d  (pin with --synth-seed %d)"
            % (args.synth_seed, args.synth_at, args.synth_every, args.synth_seed)
        )
    else:
        print(
            "[D6] moving-unit workload OFF -- this run compares an IDLE world; a transient "
            "divergence will re-converge for free and a cascading desync cannot be seen."
        )
    if args.drop_test:
        args.lobby = True  # the drop path only exists on the real-lobby host dispatch
    host_verb = "--mp-host-lobby" if args.lobby else "--mp-host"
    join_verb = "--mp-join-lobby" if args.lobby else ("--mp-join %s" % args.host_ip)

    clients = [c.strip() for c in args.clients.split(",") if c.strip()]
    if not clients:
        print("ERROR: no clients")
        return 2
    n = len(clients) + 1  # host (player 0) + N-1 clients
    rawmap = args.map or ("TUTORIAL.MP" if n == 2 else "Cold War.mpm")
    is_mpm = rawmap.lower().endswith(".mpm")
    mapname = (
        rawmap.replace(" ", "") if is_mpm else rawmap
    )  # deploy/reference space-free (scp remote paths)

    hd, key = args.host_dir, args.ssh_key
    logs = os.path.join(hd, "logs")
    vm_fwd = args.vm_dir.replace("\\", "/")
    scratch = os.path.join(os.environ.get("TEMP", "."), "mp_run")
    os.makedirs(scratch, exist_ok=True)
    host_focus = os.path.join(hd, "mh.focus.exe")
    JOIN = "mprun_join.cmd"
    print(
        "[*] N-player run: N=%d  host=%s  clients=%s  map=%s%s"
        % (n, args.host_ip, clients, mapname, "" if mapname == rawmap else " (from '%s')" % rawmap)
    )

    for ip in clients:
        if "ok" not in ssh(key, args.vm_user, ip, "echo ok").stdout:
            print("ERROR: client %s SSH failed" % ip)
            return 2

    # always (re)build the focus-patched host exe -- the run MUST use it (else a focus loss stalls it)
    print("[1] building focus-patched exe (run-without-focus, Phase 5a) ...")
    build_focus(os.path.join(hd, "mh.mp.exe"), host_focus)

    # A .mpm force-entry map must exist in <host>\Maps\ (space-free copy) + be deployed to every client.
    host_map = None
    if is_mpm:
        src = os.path.join(hd, "Maps", rawmap)
        host_map = os.path.join(hd, "Maps", mapname)
        if not os.path.isfile(host_map):
            if not os.path.isfile(src):
                print("ERROR: MP map not found on host: %s" % src)
                return 2
            shutil.copy2(src, host_map)

    # back up host inis for restore
    # ONE config file since fork F2G -- the [harness] block lives inside mh_net.ini. A stale
    # mh_harness.ini beside the exe would now be REFUSED by the DLL at boot, so sweep one if this
    # host predates the merge rather than letting the run die with a config refusal.
    net_ini_p = os.path.join(hd, "mh_net.ini")
    stale_harn = os.path.join(hd, "mh_harness.ini")
    if os.path.isfile(stale_harn):
        os.remove(stale_harn)
    backups = {p: p + ".mprun.bak" for p in (net_ini_p,) if os.path.isfile(p)}
    for p, b in backups.items():
        shutil.copy2(p, b)

    if os.path.isfile(DLL):
        shutil.copy2(DLL, os.path.join(hd, "mh.dll"))
    # THE SATELLITES (fork F4B). mh_net.dll IS the transport since the split, so a force-entry run
    # that deploys mh.dll and not its sibling would boot both peers into the no-module
    # configuration -- the MP browser explaining itself, and a determinism run with nothing to
    # exchange. Fatal rather than best-effort, unlike the mh.dll copy above, because the failure is
    # silent at every other level: the game starts, the menu renders, and only the wire is missing.
    for _sat in SATELLITES:
        _src = os.path.join(os.path.dirname(DLL), _sat)
        if not os.path.isfile(_src):
            sys.exit("mp_run: no %s beside mh.dll -- build the mh.sln first" % _sat)
        shutil.copy2(_src, os.path.join(hd, _sat))

    # Link security (2026-07-25): the transport now authenticates + encrypts with the pre-shared key in
    # mh_key.txt, so every peer in a run MUST hold the same one -- otherwise the client is refused and
    # the run fails with a bare "never connected". Peers each generate their own key on first launch,
    # which is exactly the wrong thing for a rig, so pin one here and deploy it with the DLL. This
    # exercises the SHIPPED path (handshake + encryption on) rather than testing something players
    # won't run; pass --open-link to A/B against the plaintext wire instead.
    key_path = os.path.join(hd, "mh_key.txt")
    key_text = (
        "open\n"
        if args.open_link
        else "4d48746573746b657900000000000000000000000000000000000000deadbeef\n; rig key (tools/mp_run.py)\n"
    )
    open(key_path, "w", newline="\n").write(key_text)

    if not args.no_deploy:
        print(
            "[2] deploying dll + focus exe + inis + join cmd%s to %d client(s) ..."
            % (" + map" if is_mpm else "", len(clients))
        )
        vjoin = os.path.join(scratch, JOIN)
        with open(vjoin, "w", newline="\r\n") as f:
            f.write(
                "@echo off\ncd /d %s\nmh.focus.exe %s --skip-intro\n" % (args.vm_dir, join_verb)
            )
        # The [harness] block a client peer gets, appended to its own [net] block below -- ONE
        # file since fork F2G. Plain concatenation is correct HERE and only here: net_ini()
        # emits no [harness] section, so there is no section to duplicate. Anything composing
        # fragments from two sources must merge by section instead (ui_test.ini_merge_fragment).
        harness_block = (
            (HARNESS % (args.steps, args.record, args.order_log))
            + (SYNTH % (args.synth_move, args.synth_seed, args.synth_at, args.synth_every))
            + harness_extra(args)
        )
        for i, ip in enumerate(clients):
            # host-assign test: every client declares player_id=1 (a collision) so ONLY the host's WELCOME
            # gives them distinct slots. declared-id: each client gets its own player_id 1..N-1.
            pid = 1 if args.host_assign else (i + 1)
            dst = "%s@%s:%s" % (args.vm_user, ip, vm_fwd)
            if os.path.isfile(DLL):
                scp(key, DLL, dst + "/mh.dll")
            for _sat in SATELLITES:
                scp(key, os.path.join(os.path.dirname(DLL), _sat), dst + "/" + _sat)
            scp(key, host_focus, dst + "/mh.focus.exe")
            scp(key, vjoin, dst + "/" + JOIN)
            vn = os.path.join(scratch, "vn_%d.ini" % pid)
            open(vn, "w").write(
                net_ini(args, "client", args.host_ip, pid, n, mapname) + "\n" + harness_block
            )
            scp(key, vn, dst + "/mh_net.ini")
            ssh(key, args.vm_user, ip, "del /q %s\\mh_harness.ini 2>nul & echo x" % args.vm_dir)
            scp(
                key, key_path, dst + "/mh_key.txt"
            )  # same PSK on every peer, or the join is refused
            if is_mpm:
                ssh(
                    key,
                    args.vm_user,
                    ip,
                    "if not exist %s\\Maps md %s\\Maps & echo x" % (args.vm_dir, args.vm_dir),
                )
                scp(key, host_map, dst + "/Maps/" + mapname)

    # HOST-ONLY [harness] extras (--harness-extra-host). Deliberately not deployed to the clients: the
    # point is an ASYMMETRIC run, one peer perturbed and the rest clean, so the gate has something to
    # find. Echoed so it lands in the run transcript -- an asymmetric run must never look like a
    # symmetric one after the fact.
    host_extra = "".join(
        kv.strip() + "\n" for kv in (args.harness_extra_host or "").split(";") if kv.strip()
    )
    if host_extra:
        print("[2b] host-only [harness] extras: %s" % host_extra.replace("\n", " ").strip())
    open(net_ini_p, "w").write(
        net_ini(args, "host", "0.0.0.0", 0, n, mapname)
        + "\n"
        + (HARNESS % (args.steps, args.record, args.order_log))
        + (SYNTH % (args.synth_move, args.synth_seed, args.synth_at, args.synth_every))
        + harness_extra(args)
        + host_extra
    )

    # clean stale top-level logs everywhere
    for nm in LOGNAMES + ["mh_run.txt"]:
        try:
            os.remove(os.path.join(hd, nm))
        except OSError:
            pass
    delcmd = (
        "del /q "
        + " ".join("%s\\%s" % (args.vm_dir, nm) for nm in LOGNAMES + ["mh_run.txt"])
        + " 2>nul & echo x"
    )
    for ip in clients:
        ssh(key, args.vm_user, ip, delcmd)

    host_dir = None
    drop_ok = False
    try:
        print("[3] launching host (mh.focus.exe %s) ..." % host_verb)
        if args.desktop:
            # Start-Process cannot place a window on another desktop -- lpDesktop lives in
            # STARTUPINFO and nothing in the stdlib exposes it, which is why tools/desktop.py does
            # the CreateProcessW by hand. The host peer of an mp_run IS this machine
            # (machine.HOST_IP), so on the interactive desktop the game window competes for focus
            # with whoever is using the box and a stray click changes the run.
            # hold() first: CreateProcess onto a desktop nobody holds open fails. The handle is
            # released when this process exits, i.e. exactly when the last peer is gone.
            desktop.hold(args.desktop)
            print(
                "[rig] isolated desktop: %s -- the host window cannot reach your desktop"
                % args.desktop
            )
            desktop.spawn(host_focus, "%s --skip-intro" % host_verb, cwd=hd, desktop=args.desktop)
        else:
            ps(
                "Start-Process -FilePath '%s' -ArgumentList '%s','--skip-intro' -WorkingDirectory '%s'"
                % (host_focus, host_verb, hd)
            )
        time.sleep(6)
        print("[4] launching %d client(s) ..." % len(clients))
        for ip in clients:
            ssh(
                key,
                args.vm_user,
                ip,
                'schtasks /create /tn mprun /tr "%s\\%s" /sc once /st 00:00 /ru %s /it /f && schtasks /run /tn mprun'
                % (args.vm_dir, JOIN, args.vm_user),
            )
        if args.drop_test:
            settle = 34
            print(
                "[5] DROP test: host DWELLING in lobby; waiting %ds for the client to slot ..."
                % settle
            )
            time.sleep(settle)
            host_dir = newest_run(logs, "host")

            def _hostlog(nm):
                p = os.path.join(host_dir, nm) if host_dir else None
                return open(p, errors="replace").read() if p and os.path.isfile(p) else ""

            slotted = "player 1" in _hostlog("mh_net.log")
            print("    pre-kill: host sees the client as player 1: %s" % slotted)
            print("    killing ONLY the client (transport drop, no LEAVE) ...")
            for ip in clients:
                ssh(
                    key,
                    args.vm_user,
                    ip,
                    "taskkill /im mh.focus.exe /f 2>nul & taskkill /im mh.mp.exe /f 2>nul & echo x",
                )
            time.sleep(8)
            dl = [ln.strip() for ln in _hostlog("mh_launch.log").splitlines() if "U14" in ln]
            drop_ok = slotted and any("DROPPED" in ln for ln in dl)
            print("    post-kill: host U14 delta lines:")
            for ln in dl[-8:]:
                print("      " + ln)
        else:
            print(
                "[5] waiting for host to reach step %d (timeout %ds; don't touch any window) ..."
                % (args.steps, args.timeout)
            )
            t0 = time.time()
            killed_ingame = False
            while time.time() - t0 < args.timeout:
                time.sleep(8)
                host_dir = newest_run(logs, "host")
                hs, hdn = steps_done(host_dir)
                print("    host=%s%s" % (hs, " DONE" if hdn else ""))
                # U17: kill ONE client mid-game -> B2 fast-drops it; survivors must continue to --steps and
                # the all-pairwise analysis below must still be IDENTICAL among the survivors.
                if args.ingame_kill and not killed_ingame:
                    try:
                        cur = int(hs)
                    except (TypeError, ValueError):
                        cur = 0
                    if cur >= args.ingame_kill_at:
                        print(
                            "    [U17] in-game kill: taskkill client %s at host step %d (survivors must continue)"
                            % (args.ingame_kill, cur)
                        )
                        ssh(
                            key,
                            args.vm_user,
                            args.ingame_kill,
                            "taskkill /im mh.focus.exe /f 2>nul & echo x",
                        )
                        killed_ingame = True
                if hdn:
                    break
    finally:
        print("[6] stopping peers + restoring host inis ...")
        ps("Stop-Process -Name mh.mp,mh.focus -Force -ErrorAction SilentlyContinue")
        for ip in clients:
            ssh(
                key,
                args.vm_user,
                ip,
                "taskkill /im mh.focus.exe /f 2>nul & taskkill /im mh.mp.exe /f 2>nul & schtasks /delete /tn mprun /f 2>nul & echo x",
            )
        for p, b in backups.items():
            shutil.move(b, p)

    if args.drop_test:
        print(
            "\nDROP TEST: %s"
            % (
                "PASS -- host logged a U14 DROPPED departure on client kill (LEFT delta; game vacates)"
                if drop_ok
                else "FAIL -- no DROPPED delta in host mh_launch.log (check the run folder)"
            )
        )
        return 0 if drop_ok else 1

    print("[7] pulling %d client log folder(s) ..." % len(clients))
    client_dirs = []
    for i, ip in enumerate(clients):
        pid = i + 1
        run = ssh(key, args.vm_user, ip, "type %s\\mh_run.txt" % args.vm_dir).stdout.strip()
        run_fwd = run.replace("\\", "/").rstrip("/")
        cdst = os.path.join(scratch, "client%d" % pid)
        os.makedirs(cdst, exist_ok=True)
        for nm in LOGNAMES:
            try:
                os.remove(os.path.join(cdst, nm))
            except OSError:
                pass
            scp(key, "%s@%s:%s/%s" % (args.vm_user, ip, run_fwd, nm), os.path.join(cdst, nm))
        client_dirs.append(cdst)

    print("[8] analyzing (host + %d client(s), all-pairwise) ..." % len(clients))
    dirs = [
        d
        for d in ([host_dir] + client_dirs)
        if d and os.path.isfile(os.path.join(d, "mh_harness.log"))
    ]
    if host_dir and len(dirs) >= 2:
        subprocess.run([sys.executable, os.path.join(HERE, "mp_analyze.py")] + dirs)
        return 0
    print("    missing logs (host_dir=%s, client_dirs=%s)" % (host_dir, client_dirs))
    return 1


if __name__ == "__main__":
    import hostlock

    sys.exit(hostlock.run_rig_tool(main, "mp_run"))
