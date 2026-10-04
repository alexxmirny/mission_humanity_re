#!/usr/bin/env python3
"""u64_arms.py -- mp:U64 (HM-M6): the 4-PEER failure-case arms of host migration, driven from det_arms.py.

Reached as `python tools/det_arms.py --determinism --u64 <arm> [--u64-relay] [--u53-config1]`; this module is
the runner half (lane provisioning, argv, the incident knobs) and tools/check_u64_failure.py is the verdict.

TOPOLOGY (the rig has two VMs and this box): host = vms[0] (id 0, the HUB), client1 = vms[1], client2 = the det3
local lane, client3 = the det4 local lane (lane_alloc block `det4`). Peers of one match share the host's port
(DET3_PORT). The host plays as a LIVE, UNDEFEATED hub (mp_host_u64.txt): every incident is a knob, not a gesture.

THE KNOBS (what the plan's "shim delays" became, and why):
  * a peer's process ends abruptly:  `[harness] exit_process_at_step=N;exit_process_mode=0` (TerminateProcess) on the host
    (--harness-extra-host) or on ONE client (--harness-extra-peer, tools/ui_test.py);
  * a slow client<->client path:     `[net] mesh_test_delay_ms=D` on ONE client (--net-extra-peer). net_shim sits only on
    the client<->HUB link, and the hub is the peer that dies, so it cannot make one SURVIVOR the cheapest hub; the knob
    holds that client's mesh probes and echoes D ms, so the measured round trip of a pair i<->j is base + D_i + D_j;
  * a partition:                     every client dials tools/net_shim.py (UDP) in front of the host and a state-gated
    shim trigger sends `blackhole on` when client2's harness log reaches a step (--shim-trigger): the hub is cut off
    from ALL clients while the clients still reach each other directly (the mesh and failover traffic is peer to peer).
    A second trigger (`blackhole off`) is the old host coming back.

ARMS (name: what happens / what must hold):
  base4      no incident: 4 peers IDENTICAL, every peer logs the same epoch + ranking (the topology works at all).
  elect4     the host dies at step 600, delays (c1 60, c2 0, c3 30 ms): client2 (NOT the lowest id) must be elected.
  newhub     elect4, then the ELECTED hub dies too (step 1400): a second failover among the two left, 3 of 3 of its epoch.
  double     the host AND the rank-1 candidate die together (step 600): 2 of 4 survive, an even split -- Q1: BOTH survivors end
             ("connection lost"), consistently and promptly; nobody hangs, nobody plays on alone.
  partition  the hub is cut off from all clients at step ~700: the 3 clients elect and play on IDENTICAL, the hub (a minority
             of 1 of 4) ends.
  spec4      mp:U71: client1 is DEFEATED at step ~300 (the harness `conq` force-kill) and stays as a SPECTATOR (mp:U54); at step 600 the
             host AND the spectator die together. The survivors (client2, client3) must elect and play on IDENTICAL: the
             spectator bit (MH_Net_SetSpectator) keeps the dead spectator out of the failover quorum (2 of 3 voters, not
             an even 2 of 4).
  spec4neg   the negative control: the same incident with `[net] spectate_mask=0` (the bit is never exported), so the
             spectator is still a voter and the 2 survivors are an even split -- both must END (as `double` does).
  heal       partition, then the shim passes again at step ~1500: the old host's traffic (old conn ids) reaches the survivors;
             they must not be disturbed (IDENTICAL, no second failover, no notice).
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lane_alloc  # noqa: E402

DET4_LANE = "det4_client3"
DET4_LANE_NO = lane_alloc.lane("det4", 0)

HOST_SCRIPT = "mp_host_u64.txt"
SPECTATOR_SCRIPT = (
    "mp_client_u71_spectator.txt"  # client1 of the spec4 arms: defeated, picks Continue spectating
)
CLIENT_SCRIPTS = ("mp_client_u64_c1.txt", "mp_client_u64_c2.txt", "mp_client_u64_c3.txt")
# the step regex a `blackhole` trigger waits for in client2's harness log (hash rows: "<step> <16 hex> ...")
STEP_AT_LEAST = r"(?m)^(?:%s) [0-9A-F]{16} "


def _step_regex(n):
    """Regex matching a hashed-step row whose step is >= n (n a multiple of 100)."""
    h = n // 100
    # >= h*100: three-digit rows with a hundreds digit >= h (h < 10), or any four-plus-digit row
    if h <= 9:
        return STEP_AT_LEAST % (r"[%d-9]\d\d|\d{4,}" % h)
    return STEP_AT_LEAST % r"\d{4,}"


EXIT_STEP = 600
# The delays that make client2 the cheapest hub among {c1, c2, c3}: edges c1c2 60, c2c3 30, c1c3 90 ms (+ the LAN base).
# cost(c) = the two largest edges at c: c1 150, c2 90, c3 120 -> c2, then c3, then c1.
DELAYS = {1: 60, 2: 0, 3: 30}

ARMS = {
    "base4": dict(
        steps=900,
        crash={},
        delay={},
        survivors=("host", "client1", "client2", "client3"),
        exclude=(),
    ),
    "elect4": dict(
        steps=3000,
        crash={"host": EXIT_STEP},
        delay=DELAYS,
        survivors=("client1", "client2", "client3"),
        exclude=("host",),
        expect_hub=2,
    ),
    "newhub": dict(
        steps=2600,
        crash={"host": EXIT_STEP, "client2": 1400},
        delay=DELAYS,
        survivors=("client1", "client3"),
        exclude=("host", "client2"),
        expect_hub=2,
        second_failover=True,
    ),
    "double": dict(
        steps=2400,
        crash={"host": EXIT_STEP, "client2": EXIT_STEP},
        delay=DELAYS,
        survivors=("client1", "client3"),
        exclude=("host", "client2"),
        minority=True,
    ),
    "partition": dict(
        steps=3000,
        crash={},
        delay=DELAYS,
        survivors=("client1", "client2", "client3"),
        exclude=("host",),
        expect_hub=2,
        partition_at=700,
        minority_peers=("host",),
    ),
    "spec4": dict(
        steps=3000,
        crash={"host": EXIT_STEP, "client1": EXIT_STEP},
        delay=DELAYS,
        survivors=("client2", "client3"),
        exclude=("host", "client1"),
        expect_hub=2,
        spectator="client1",
        conq=True,
    ),
    "spec4neg": dict(
        steps=2400,
        crash={"host": EXIT_STEP, "client1": EXIT_STEP},
        delay=DELAYS,
        survivors=("client2", "client3"),
        exclude=("host", "client1"),
        minority=True,
        spectator="client1",
        conq=True,
        net_extra="spectate_mask=0",
        neg_control=True,  # the minority checks that assume an even-split loss from step 600 on tolerate the conq harness tail
    ),
    "heal": dict(
        steps=3600,
        crash={},
        delay=DELAYS,
        survivors=("client1", "client2", "client3"),
        exclude=("host",),
        expect_hub=2,
        partition_at=700,
        heal_at=1000,
        minority_peers=("host",),
    ),
}


def _lane_cmd(det_arms, name, no, config1, cfg):
    cmd = [
        sys.executable,
        os.path.join(det_arms.REPO, "tools", "make_lane.py"),
        "--name",
        name,
        "--lane",
        str(no),
        "--port",
        str(det_arms.DET3_PORT),
        "--headless",
    ]
    if config1:
        cmd += ["--omit-satellite", "libmh.dll"]
    if not cfg.stock_exe:
        cmd.append("--patched-exe")
    return cmd


def run(args, cfg, arm, config1=False, relayed=False):
    """One U64 arm. Returns (ok, lines), the run_u53 contract (ok None = SKIP)."""
    import det_arms as D
    import check_u64_failure as chk

    spec = ARMS[arm]
    det_dir = D.rig_phases.det_dir_path()
    down = [ip for ip in args.vms[:2] if not D.vm_reachable(ip)]
    if down:
        return None, ["      SKIP -- VM(s) unreachable: %s" % ", ".join(down)]
    for name, no in ((D.DET3_LANE, D.DET3_LANE_NO), (DET4_LANE, DET4_LANE_NO)):
        r = subprocess.run(_lane_cmd(D, name, no, config1, cfg), capture_output=True, text=True)
        if r.returncode != 0:
            return False, ["      FAIL: lane %s: %s" % (name, (r.stderr or r.stdout).strip()[:200])]
    steps = getattr(args, "u64_steps", None) or spec["steps"]
    crash = dict(spec["crash"])
    if getattr(args, "u64_exit_step", None):
        crash = {k: args.u64_exit_step if v == EXIT_STEP else v for k, v in crash.items()}
    host_extra = None
    if "host" in crash:
        host_extra = "exit_process_at_step=%d;exit_process_mode=0" % crash["host"]
    if spec.get("conq"):
        # mp:U71: the harness conq workload force-kills slot 1 (client1) at step 60 + U53_KILL_D, defeating it
        conq = "conq=1;conq_at=60;conq_force_kill_at=%d;conq_probe_every=50" % D.U53_KILL_D
        host_extra = conq if not host_extra else host_extra + ";" + conq
    peer_extra = [
        "%s:exit_process_at_step=%d;exit_process_mode=0" % (p[-1], s)
        for p, s in sorted(crash.items())
        if p != "host"
    ]
    net_peer = ["%d:mesh_test_delay_ms=%d" % (i, d) for i, d in sorted(spec["delay"].items()) if d]
    shim = None
    if spec.get("partition_at"):
        trig = [
            {
                "cmd": "blackhole on",
                "when": [["c2", "mh_harness.log", _step_regex(spec["partition_at"])]],
            }
        ]
        if spec.get("heal_at"):
            trig.append(
                {
                    "cmd": "blackhole off",
                    "after_prev": True,  # mp:U65: never in the same poll as the cut
                    # evidence-gated, not step-gated: the det poll is 8 s, the sim runs ~100 steps/s, so a step bound can
                    # fire in the SAME poll as the cut (the first heal arm: a 0.5 s blip, no failover at all). The heal
                    # waits until the new hub's failover is over on the game side, and still lands inside the old host's
                    # 10 s link timeout.
                    "when": [["c2", "mh_net.log", r"U63 failover over"]],
                }
            )
        shim = {"target": args.vms[0], "delay": 0, "triggers": trig}
    relay_cm = (
        D.RelayProc(D.RELAY_PORT, os.path.join(D.REPO, "tmp", "relay_u64.log"))
        if relayed
        else D.contextlib.nullcontext()
    )
    with relay_cm as relay:
        net_parts = ["peers=3", "transport=udp"]
        if relayed:
            if not relay.ok:
                return None, ["      SKIP -- the relay could not be started: %s" % relay.note]
            net_parts += [
                "relay=%s:%d" % (D.relay_addr_for_peers(False), relay.port),
                "force_relay=1",
            ]
        if spec.get("net_extra"):
            net_parts.append(spec["net_extra"])
        if args.net_extra:
            net_parts.append(args.net_extra)
        argv = D.build_scenario_argv(
            cfg=cfg,
            determinism=True,
            steps=steps,
            host="%s:%s" % (args.vms[0], HOST_SCRIPT),
            clients=[
                "%s:%s"
                % (args.vms[1], SPECTATOR_SCRIPT if spec.get("spectator") else CLIENT_SCRIPTS[0]),
                "lane=%s:%s"
                % (
                    D.DET3_LANE,
                    "mp_client_u71_c2.txt" if spec.get("spectator") else CLIENT_SCRIPTS[1],
                ),
                "lane=%s:%s"
                % (
                    DET4_LANE,
                    "mp_client_u71_c3.txt" if spec.get("spectator") else CLIENT_SCRIPTS[2],
                ),
            ],
            det_exclude=list(spec["exclude"]),
            connect_ip=args.vms[0],
            timeout_frames=D.LOCAL_TIMEOUT_FRAMES * 4,
            net_extra=";".join(net_parts),
            timeout=max(args.timeout, 600 + steps),
            extra_ini=[] if config1 else [D.PROMOTE_INI],
            harness_extra=D.with_det_hash_kind(
                "region_hash_step=1;synth_move=0;gameover_step=100000;gameover_stop=0"
                if spec.get("conq")
                else "region_hash_step=1"
            ),
            harness_extra_host=host_extra,
            harness_extra_peer=peer_extra,
            net_extra_peer=net_peer,
            shim=shim,
            omit_satellite=["libmh.dll"] if config1 else (),
        )
        D.det_clear(det_dir)
        rc = D.run_ui_test(argv, max(args.per_test_timeout, 900 + steps))[0]
    want = {p: not config1 for p in ("host", "client1", "client2", "client3")}
    if config1:
        ok, lines = D.det_run_report(
            det_dir,
            {p: False for p in want},
            configs={p: "1" for p in want},
        )
    else:
        ok, lines = D.det_run_report(det_dir, want)
    try:
        qok, qlines = chk.check(det_dir, arm, spec, crash, relayed)
    except chk.u53.Refusal as exc:
        qok, qlines = False, ["REFUSED: %s" % exc]
    # MEASUREMENT-shaped like the u55/u62/u63 exit arms: a dead peer's frozen step count aborts the runner by design,
    # so the checker's verdict is the verdict (base4 has no dead peer: the runner's own verdict counts too).
    verdict = qok and (rc == 0 and ok if arm == "base4" else True)
    if not verdict:
        # keep the RED's evidence: the next det_clear wipes it, and a 1-in-3 flake is only diagnosable from its own logs
        import shutil
        import time as _t

        keep = os.path.join(
            D.REPO,
            "tmp",
            "u64_red",
            "%s%s-%s" % (arm, "-relay" if relayed else "", _t.strftime("%H%M%S")),
        )
        try:
            shutil.copytree(det_dir, keep)
            qlines = qlines + ["artifacts kept at %s" % keep]
        except OSError:
            pass
    return verdict, lines + [
        "   check_u64_failure (%s%s):" % (arm, "-relay" if relayed else "")
    ] + ["      " + ln for ln in qlines]
