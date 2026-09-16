#!/usr/bin/env python3
# tools/mp_prep_manual.py -- prep the peers for a MANUAL (hand-clicked) N-player MP test through the real
# UX: main menu -> Multiplayer -> Create (host) / Discover+Join (clients) -> lobby -> Start -> game.
#
# This is the manual counterpart of tools/mp_run.py (which drives the lobby programmatically via verbs). It
# DEPLOYS ONLY -- current mh.dll + a fresh run-without-focus exe + host_assign manual inis to the host and
# each client -- then prints the per-peer click sequence. It does NOT launch anything: the whole point is to
# exercise the live menus by hand.
#
# Manual-path facts (why the ini differs from mp_run's):
#   * host_assign=1 on ALL peers -- the host assigns each joiner a distinct id over the wire (FLAG_WELCOME),
#     so clients that all default to player_id=1 still get seated at distinct slots (N1). Without it two
#     hand-clicked clients would both be player 1.
#   * NO mp_players -- the manual path is OCCUPANCY-driven: the host enters on the Start button (which uses
#     the live occupied-slot count) and the client waits for the host's Start, so N = who actually joined.
#     (mp_players only matters for the automated --lobby path, which auto-enters at occ>=N with no Start.)
#   * NO launch verb -- pure menu. The DLL's manual-lobby driver arms when you click through Multiplayer.
#   * The map is picked by the HOST in the Create map-list (Maps\*.mpm); for >2 players pick a map DESIGNED
#     for >=N players (Blue Monday / Moby Dick / Island Warfare / White Peace = 8p, Playground = 6p). Both
#     the host and every client need that .mpm in their Maps\ (the client loads its body locally; the header
#     arrives via host_send_map). The maps are already deployed on the standing VMs.
#
# Usage:
#   python tools/mp_prep_manual.py [--clients <peerA>,<peerB>] [--host-ip <host>]
#       [--host-dir <polygon>] [--vm-user <user>] [--vm-dir <vm-dir>] [--ssh-key ...]
#   (machine-specific defaults live in tools/machine_config.py)

import argparse, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import machine_config as machine  # noqa: E402
import mp_run  # reuse ssh / scp / build_focus / DLL

# Manual [net] ini. host = bind addr (0.0.0.0) on the host, the host IP on a client (its connect target).
# Perf knobs mirror mp_run's proven defaults (fps-cap-friendly lockstep). A light [harness] block hashes
# every step without ever stopping the game (exit_on_stop=0, huge stop_step) so a manual run STILL leaves a
# determinism trail in the logs -- purely a bonus; delete it for a pure play session.
NET = (
    "[net]\nhost=%s\nport=6501\nhost_assign=1\nlog=1\n"
    "lockstep_step_ms=30\nlockstep_step_eps_ms=0.01\nsim_step_ms=10\nrx_spin=0\n"
    "horizon_heartbeat_ms=50\ndefang_overlay=1\nlog_gamemode=0\ngame_speed_pct=0\n"
    "eager_advertise=1\nhires_clock=1\nqpc_clock=1\nlockstep_log=1\nbootstrap=1\n"
)
# `enable=1` is the arm (fork F2G): the block rides inside mh_net.ini now and the harness no
# longer arms off a separate file existing. Drop the whole block for a pure play session.
HARNESS = (
    "[harness]\nenable=1\nseed_step=0\nseed_mode=2\nstop_step=100000000\nexit_on_stop=0\n"
    "fixed_step=0\npin_fpu=1\nregion_hash_step=1\norder_mode=0\norder_log=0\n"
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clients", default=f"{machine.RIG_PEER_A},{machine.RIG_PEER_B}")
    ap.add_argument("--host-ip", default=machine.HOST_IP)
    ap.add_argument("--host-dir", default=machine.POLYGON)
    ap.add_argument("--vm-user", default=machine.VM_USER)
    ap.add_argument("--vm-dir", default=machine.VM_DIR)
    ap.add_argument("--ssh-key", default=machine.SSH_KEY)
    args = ap.parse_args()

    clients = [c.strip() for c in args.clients.split(",") if c.strip()]
    hd, key = args.host_dir, args.ssh_key
    vm_fwd = args.vm_dir.replace("\\", "/")
    scratch = os.path.join(os.environ.get("TEMP", "."), "mp_prep")
    os.makedirs(scratch, exist_ok=True)
    host_focus = os.path.join(hd, "mh.focus.exe")

    for ip in clients:
        if "ok" not in mp_run.ssh(key, args.vm_user, ip, "echo ok").stdout:
            print("ERROR: client %s SSH failed" % ip)
            return 2

    print("[1] building run-without-focus exe ...")
    mp_run.build_focus(os.path.join(hd, "mh.mp.exe"), host_focus)

    if not os.path.isfile(mp_run.DLL):
        print("ERROR: built mh.dll not found: %s (build the solution first)" % mp_run.DLL)
        return 2

    print("[2] host (%s): mh.dll + manual inis ..." % args.host_ip)
    import shutil

    shutil.copy2(mp_run.DLL, os.path.join(hd, "mh.dll"))
    # ONE file (fork F2G): [net] + [harness] in the same mh_net.ini. net_ini emits no
    # [harness] section, so plain concatenation cannot produce a duplicate block here.
    open(os.path.join(hd, "mh_net.ini"), "w").write(
        (NET % "0.0.0.0") + "\n" + HARNESS
    )  # host: bind all
    stale = os.path.join(hd, "mh_harness.ini")  # refused by the DLL if it survives
    if os.path.isfile(stale):
        os.remove(stale)

    for ip in clients:
        print("[3] client %s: mh.dll + focus exe + manual inis ..." % ip)
        dst = "%s@%s:%s" % (args.vm_user, ip, vm_fwd)
        mp_run.scp(key, mp_run.DLL, dst + "/mh.dll")
        mp_run.scp(key, host_focus, dst + "/mh.focus.exe")
        vn = os.path.join(scratch, "net_%s.ini" % ip.replace(".", "_"))
        # client: connect target = the host IP
        open(vn, "w").write((NET % args.host_ip) + "\n" + HARNESS)
        mp_run.scp(key, vn, dst + "/mh_net.ini")
        mp_run.ssh(key, args.vm_user, ip, "del /q %s\\mh_harness.ini 2>nul & echo x" % args.vm_dir)

    print(
        """
================================================================================
PREP DONE. Now launch each peer BY HAND (double-click or run mh.focus.exe --skip-intro
in its game dir) and click through the real menus -- do NOT pass any --mp verb.

  HOST (%s, this box):
    1. Multiplayer -> Create Game
    2. pick an >=N-player map from the list (e.g. "Blue Monday" for 3 players)
    3. you're in the lobby -- wait until BOTH clients appear as filled slots
    4. click START

  EACH CLIENT (%s):
    1. Multiplayer -> (discover / join) -- select the host's game (or type %s)
    2. you're in the lobby (its own slot filled) -- just wait
    3. when the host clicks Start, you auto-enter the game

WATCH FOR (in each peer's logs\\<run>\\):
    mh_net.log     : host -> "assigned conn N -> player K (WELCOME sent)" (x%d);
                     each client -> "WELCOME -- host assigned us player K" with DISTINCT K.
    mh_launch.log  : each peer "TRIGGERING game entry ... map_pcount->%d" then enters mode-3.
    mh_harness.log : all peers hash steps; run tools/mp_analyze.py on the 3 run folders to
                     confirm the pairwise region-hashes are identical (bonus determinism check).
================================================================================"""
        % (args.host_ip, ", ".join(clients), args.host_ip, len(clients), len(clients) + 1)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
