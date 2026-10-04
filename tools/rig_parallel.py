#!/usr/bin/env python3
"""rig_parallel.py -- tooling:TL-RIG-PARALLEL: run several independent det_arms.py rig arms N-wide.

    python tools/rig_parallel.py --parallel 4 --preset u63
    python tools/rig_parallel.py --parallel 3 --arm=--u63-crash "--arm=--u63-crash --u55-relay" --arm=--u54-neg
    python tools/rig_parallel.py --parallel 1 --topology vm --preset u63      # the serial VM baseline (status quo)

An ARM is a det_arms.py flag string for a 3-peer shape (`--u63-crash`, `--u54-client-spec --u55-relay`, `--u53-config1
--u63-crash`, ...); `--determinism` is added for you. Each arm becomes ONE `python tools/det_arms.py --determinism
<arm> --local-peers --par-slot K` child, and K is what isolates it (nothing here is a second scheduler -- the lane,
port and dir machinery is the existing one, parameterised by the slot):

  lanes     lane_alloc block `par`: slot K owns 4 lane numbers (= 4 single-instance mutexes), provisioned as
            workdir/mh_lanes/par<K>_{host,c1,c2} by make_lane.py (links, not copies, when the junction fallback holds)
  ports     the slot's game port is PORT_BASE + its first lane (all peers of one match share it, as the VM topology
            does with 6501); a relayed arm's mh_relay binds an EPHEMERAL port and logs under the slot's scratch
  dirs      $MH_RIG_SCRATCH = tmp/rig_par/slot<K>: pulled logs (<scratch>/determinism), red copies, poll dirs,
            relay log -- ui_test.py and det_arms.py both read the variable (rig_phases.scratch_dir)
  rig lease this wrapper takes the ONE `rig` lease; every child inherits it (hostlock re-entrancy), so a foreign rig
            run still waits for the whole batch and the batch never waits on itself
  machine-wide .boot.lock (pack-load window) is shared by all slots on purpose: it only serialises the ~seconds of a
            boot, so launches are also staggered (--stagger) to keep the queue shallow

VM-needing arms: `--topology vm` (or a spec prefixed `vm:`) runs the arm in the classic VM+VM+lane topology. There is
one pair of VMs, so those arms are serialised on a lock and take no slot; local arms run beside them.

NOT parallelisable here (the child refuses or the scheduler would lie): arms that need the shim control port singleton
(6699: the u64 partition/heal arms) or `--u64*` at all (4 local lanes + a shim) -- run those through det_arms.py directly.

Output: one verdict table (arm, slot, verdict, wall, per-phase seconds from the child's `[phase]` lines) and
tmp/rig_par/last_run.json. Exit 0 only if every arm is PASS.
"""

import argparse
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lane_alloc  # noqa: E402

PRESETS = {
    # mp:U63 -- 4 crash shapes + the negative control (done_when of TL-RIG-PARALLEL)
    "u63": [
        "--u63-crash",
        "--u63-crash --u55-relay",
        "--u63-crash --u53-config1",
        "--u63-crash --u55-relay --u53-config1",
        "--u63-neg",
    ],
}
VERDICT_RE = re.compile(r"\[det\] U\d\d (\S+): (PASS|FAIL)")
PHASE_RE = re.compile(
    r"^\[phase\] (provision|boot|menu_walk|clients|lobby|match|pull|analyze|TOTAL)\s+([0-9.]+)",
    re.M,
)
# a failed arm is labelled LOAD-SUSPECT when its log carries the signatures a starved box produces (a peer that
# never presented a frame, the boot lock queue, a watchdog timeout) -- the judgment stays with the reader.
LOAD_SIGNS = ("did not present a frame", "waited", "TIMEOUT", "timed out", "stalled run")
_procs = {}
_procs_lock = threading.Lock()


def kill_tree(proc):
    try:
        subprocess.run(
            ["taskkill", "/PID", str(proc.pid), "/T", "/F"], capture_output=True, timeout=30
        )
    except (OSError, subprocess.SubprocessError):
        pass


def run_arm(idx, spec, slot, topology, out_dir, timeout, py):
    """Run one arm child to completion. Returns the result dict."""
    vm = topology == "vm"
    flags = spec.split()
    cmd = [py, os.path.join(REPO, "tools", "det_arms.py"), "--determinism"] + flags
    if not vm:
        cmd += ["--local-peers", "--par-slot", str(slot)]
    log = os.path.join(out_dir, "arm%02d.log" % idx)
    t0 = time.time()
    rc, why = None, ""
    with open(log, "w", encoding="utf-8", errors="replace") as fh:
        fh.write("$ %s\n" % " ".join(cmd))
        fh.flush()
        proc = subprocess.Popen(cmd, cwd=REPO, stdout=fh, stderr=subprocess.STDOUT)
        with _procs_lock:
            _procs[idx] = proc
        try:
            rc = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            kill_tree(proc)
            proc.wait()
            rc, why = -1, "arm timeout %ds (killed)" % timeout
    wall = time.time() - t0
    with open(log, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    m = VERDICT_RE.findall(text)
    verdict = m[-1][1] if m else ("SKIP" if "SKIP" in text and rc == 0 else "FAIL")
    if rc not in (0, None) and verdict == "PASS":
        verdict = "FAIL"
    phases = {}
    for k, v in PHASE_RE.findall(text):
        phases.setdefault(k, float(v))
    suspect = [s for s in LOAD_SIGNS if s in text] if verdict != "PASS" else []
    return dict(
        idx=idx,
        arm=spec,
        slot=None if vm else slot,
        topology=topology,
        verdict=verdict,
        rc=rc,
        wall_s=round(wall, 1),
        phases=phases,
        note=why,
        load_suspect=suspect,
        log=os.path.relpath(log, REPO),
    )


def schedule(specs, n, topology, out_dir, stagger, timeout, py):
    """N-wide: `n` worker threads, each arm takes a free slot (local) or the VM lock (vm topology)."""
    slots = queue.Queue()
    for k in range(min(n, lane_alloc.PAR_SLOTS)):
        slots.put(k)
    vm_lock = threading.Lock()
    work = queue.Queue()
    arms = []
    for i, s in enumerate(specs):
        arm_topo = topology
        if s.startswith("vm:"):
            s, arm_topo = s[3:], "vm"
        arms.append((i, s.strip(), arm_topo))
    # VM arms first: they are the serialised resource, so starting them early keeps the local arms' tail short
    arms.sort(key=lambda a: a[2] != "vm")
    for a in arms:
        work.put(a)
    results, rlock = [], threading.Lock()
    start_gate = threading.Lock()
    last_start = [0.0]

    def worker():
        while True:
            try:
                idx, spec, topo = work.get_nowait()
            except queue.Empty:
                return
            slot = None
            if topo == "vm":
                vm_lock.acquire()
            else:
                slot = slots.get()
            try:
                with start_gate:  # stagger launches so boots do not all hit the .boot.lock at once
                    wait = last_start[0] + stagger - time.time()
                    if wait > 0:
                        time.sleep(wait)
                    last_start[0] = time.time()
                print(
                    "[par] start arm %d%s: %s"
                    % (idx, "" if slot is None else " (slot %d)" % slot, spec),
                    flush=True,
                )
                r = run_arm(idx, spec, slot, topo, out_dir, timeout, py)
            finally:
                if topo == "vm":
                    vm_lock.release()
                else:
                    slots.put(slot)
            print(
                "[par] done  arm %d %s %s in %.0f s" % (idx, spec, r["verdict"], r["wall_s"]),
                flush=True,
            )
            with rlock:
                results.append(r)

    ths = [threading.Thread(target=worker, daemon=True) for _ in range(max(1, n))]
    for t in ths:
        t.start()
    try:
        for t in ths:
            while t.is_alive():
                t.join(timeout=1.0)
    except KeyboardInterrupt:
        with _procs_lock:
            for p in _procs.values():
                kill_tree(p)
        raise
    return sorted(results, key=lambda r: r["idx"])


def print_table(results, wall, n):
    cols = ("provision", "boot", "menu_walk", "clients", "lobby", "match", "pull", "analyze")
    print("\n" + "=" * 118)
    print("[par] VERDICT TABLE -- %d arm(s), %d-wide, batch wall %.0f s" % (len(results), n, wall))
    print("=" * 118)
    print(
        "  %-2s %-4s %-44s %-8s %6s  %s"
        % ("#", "slot", "arm", "verdict", "wall", " ".join("%5s" % c[:5] for c in cols))
    )
    for r in results:
        ph = r["phases"]
        print(
            "  %-2d %-4s %-44s %-8s %6.0f  %s"
            % (
                r["idx"],
                "vm" if r["slot"] is None else r["slot"],
                r["arm"][:44],
                r["verdict"],
                r["wall_s"],
                " ".join("%5.0f" % ph.get(c, 0.0) for c in cols),
            )
        )
        if r["note"]:
            print("       note: %s" % r["note"])
        if r["load_suspect"]:
            print(
                "       LOAD-SUSPECT signatures in %s: %s"
                % (r["log"], ", ".join(r["load_suspect"]))
            )
    tot = sum(r["wall_s"] for r in results)
    print(
        "  sum of arm walls %.0f s -> batch wall %.0f s (x%.2f)"
        % (tot, wall, tot / wall if wall else 0)
    )


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "arms",
        nargs="*",
        help="det_arms flag strings, one per arm; a string that starts with `--` must be written --arm=SPEC",
    )
    ap.add_argument("--arm", action="append", default=[], help="one arm (--arm=--u63-crash)")
    ap.add_argument(
        "--preset", action="append", default=[], choices=sorted(PRESETS), help="add a named arm set"
    )
    ap.add_argument(
        "--parallel", "-j", type=int, default=3, help="arms at once (1..%d)" % lane_alloc.PAR_SLOTS
    )
    ap.add_argument(
        "--topology",
        choices=("local", "vm"),
        default="local",
        help="local = all-local lanes (default, N-wide); vm = classic VM+VM+lane arms, serialised",
    )
    ap.add_argument(
        "--stagger",
        type=float,
        default=8.0,
        help="seconds between arm launches (boot-lock queue depth)",
    )
    ap.add_argument(
        "--arm-timeout", type=int, default=1500, help="kill an arm child after this many seconds"
    )
    ap.add_argument("--tag", default="", help="label for the run's output dir / json")
    ap.add_argument("--list", action="store_true", help="print the presets and exit")
    args = ap.parse_args(argv)
    if args.list:
        for k, v in PRESETS.items():
            print("%s: %s" % (k, " | ".join(v)))
        return 0
    specs = [s for p in args.preset for s in PRESETS[p]] + list(args.arm) + list(args.arms)
    if not specs:
        ap.error("give arms or --preset")
    for s in specs:
        if "--u64" in s or "--shim" in s:
            ap.error(
                "arm %r: u64/shim arms are not parallelisable (shim control port 6699 singleton) -- run them via det_arms.py"
                % s
            )
    if not 1 <= args.parallel <= lane_alloc.PAR_SLOTS:
        ap.error("--parallel must be 1..%d" % lane_alloc.PAR_SLOTS)
    n = (
        1
        if args.topology == "vm" and not any(s.startswith("vm:") for s in specs)
        else args.parallel
    )
    stamp = time.strftime("%Y%m%dT%H%M%S")
    out_dir = os.path.join(
        REPO, "tmp", "rig_par", "run_%s%s" % (stamp, "_" + args.tag if args.tag else "")
    )
    os.makedirs(out_dir, exist_ok=True)
    print(
        "[par] %d arm(s), %d-wide, topology %s, logs in %s"
        % (len(specs), n, args.topology, os.path.relpath(out_dir, REPO)),
        flush=True,
    )
    t0 = time.time()
    results = schedule(
        specs,
        n,
        args.topology,
        out_dir,
        args.stagger if n > 1 else 0.0,
        args.arm_timeout,
        sys.executable,
    )
    wall = time.time() - t0
    print_table(results, wall, n)
    with open(os.path.join(out_dir, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(
            dict(parallel=n, topology=args.topology, batch_wall_s=round(wall, 1), arms=results),
            f,
            indent=1,
        )
    with open(os.path.join(REPO, "tmp", "rig_par", "last_run.json"), "w", encoding="utf-8") as f:
        json.dump(
            dict(
                parallel=n,
                topology=args.topology,
                batch_wall_s=round(wall, 1),
                arms=results,
                dir=out_dir,
            ),
            f,
            indent=1,
        )
    ok = all(r["verdict"] == "PASS" for r in results)
    print("[par] %s" % ("ALL PASS" if ok else "NOT ALL PASS"))
    return 0 if ok else 1


if __name__ == "__main__":
    import hostlock

    if {"--list", "-h", "--help"} & set(sys.argv[1:]):
        raise SystemExit(main())
    raise SystemExit(hostlock.run_rig_tool(main, "rig_parallel"))
