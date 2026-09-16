#!/usr/bin/env python3
# tools/mp_step_sweep.py -- the sub-millisecond lockstep_step_ms sweep (100 + epsilon): does +1 ms fix the
# phase, and is the transition a hard threshold or a monotone ramp? (the
# "sub-ms step sweep" open item; sibling of mp_run_sweep.py which sweeps fps instead.)
#
# Question: step_ms=100 runs measurably worse than 101 (wall-clock deficit ~6.5s vs ~3.3s over 800 steps).
# A naive quantum-grid argument predicts 101 should need an 11th 10ms quantum (=> WORSE) and it doesn't.
# This sweep varies step_ms in fractional-ms increments to map the curve between 100 and 101.
#
# Enabled by the 2026-07-20 parse widening: net_seams.cpp now atof()s lockstep_step_ms (was int-only), and
# mp_run.py --step-ms is float. g_lockstep_step is a double in game-seconds, pinned verbatim each frame.
#
# Experiment design (from the task's done_when):
#   * FPSLimit is forced UNSET on both peers for the whole sweep (so the fps-cap fix doesn't confound the
#     read) -- backed up + restored around the sweep, exactly like mp_run_sweep.py.
#   * knobs otherwise default: --qpc 1 --hires 1 --eager 1 --temporal 1 (mp_run.py defaults).
#   * PRIMARY metric = total wall-clock deficit over the FULL run (rate_full = step_ms / mean_step_delta,
#     i.e. ideal_elapsed / actual_elapsed), NOT a tail window -- the 2026-07-20 warm-up-transient lesson:
#     per-step wall time is additive, a slow start is a permanent addition, so a tail/marginal rate lies.
#   * SECONDARY = per-step delta distribution shape (slow-mode fraction, mean vs median) as a mechanism
#     clue (bimodal => phase/aliasing; unimodal-shifted => uniform tax). Raw deltas dumped per run.
#   * fps is recorded per run (TEV_FRAME count / wall span) so the "loss is fps-independent" fact stays
#     falsifiable -- frame rate is NOT controlled, only observed.
#
# Usage:
#   python tools/mp_step_sweep.py [--steps 800] [--eps 0,0.1,0.25,0.5,0.75,1.0] [--base-ms 100]
#       [--out tmp/step_sweep.csv] [--start-at 0] [--limit N]
#       [--host-dir <polygon>] [--vm-ip <vm>] [--vm-user <user>] [--vm-dir <vm-dir>] [--ssh-key <key>]
#   (machine-specific defaults live in tools/machine_config.py)

import argparse, csv, glob, os, re, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import machine_config as machine  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MP_RUN = os.path.join(HERE, "mp_run.py")
TEV_FRAME = 0
TEV_SIMSTEP = 5


def sh(cmd, timeout=None):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


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


def scp(key, src, dst, timeout=60):
    def norm(p):
        return p if "@" in p else p.replace("\\", "/")

    return sh(
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


def newest_run(logs_dir, role):
    c = [d for d in glob.glob(os.path.join(logs_dir, "*_" + role)) if os.path.isdir(d)]
    c.sort(key=os.path.getmtime, reverse=True)
    return c[0] if c else None


def set_fps_limit(path, value):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()
    newval = "" if value is None else str(value)
    content2, n = re.subn(r"(?m)^(FPSLimit\s*=\s*).*$", lambda m: m.group(1) + newval, content)
    if n != 1:
        raise RuntimeError("FPSLimit line not found (or matched %d times) in %s" % (n, path))
    with open(path, "w", encoding="utf-8") as f:
        f.write(content2)


def clean_peers(key, user, ip, vm_dir):
    sh(
        [
            "powershell",
            "-NoProfile",
            "-Command",
            "Stop-Process -Name mh.focus,mh.mp -Force -ErrorAction SilentlyContinue",
        ]
    )
    ssh(key, user, ip, "taskkill /im mh.focus.exe /f 2>nul & echo x")
    ssh(key, user, ip, "taskkill /im mh.mp.exe /f 2>nul & echo x")
    ssh(key, user, ip, "schtasks /delete /tn mprun /f 2>nul & echo x")


def read_temporal(temporal_log_path):
    """Return (simstep_qpcs, frame_qpcs) in raw QPC-microsecond units (parts[0] is already us)."""
    simstep, frame = [], []
    if not os.path.isfile(temporal_log_path):
        return simstep, frame
    with open(temporal_log_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                qpc = int(parts[0])
                eid = int(parts[1])
            except ValueError:
                continue
            if eid == TEV_SIMSTEP:
                simstep.append(qpc)
            elif eid == TEV_FRAME:
                frame.append(qpc)
    return simstep, frame


def summarize(simstep_qpcs, frame_qpcs, step_ms):
    """Honest full-run total-deficit metric + distribution shape + observed fps."""
    if len(simstep_qpcs) < 2:
        return {
            "n": 0,
            "mean_ms": None,
            "median_ms": None,
            "rate_full": None,
            "deficit_s": None,
            "slow_frac": None,
            "fps": None,
        }
    deltas = [(b - a) / 1000.0 for a, b in zip(simstep_qpcs, simstep_qpcs[1:])]  # ms
    n = len(deltas)
    mean_all = sum(deltas) / n
    sd = sorted(deltas)
    median = sd[n // 2] if n % 2 else (sd[n // 2 - 1] + sd[n // 2]) / 2.0
    # total wall-clock deficit over the FULL run (the honest metric)
    span_ms = (simstep_qpcs[-1] - simstep_qpcs[0]) / 1000.0
    ideal_ms = n * step_ms
    rate_full = ideal_ms / span_ms if span_ms else None
    deficit_s = (span_ms - ideal_ms) / 1000.0
    # slow-mode fraction: steps taking >1.5x the nominal (mechanism-shape clue)
    slow_frac = sum(1 for d in deltas if d > 1.5 * step_ms) / n
    # observed fps over the SAME simstep span
    fps = None
    if frame_qpcs and span_ms > 0:
        lo, hi = simstep_qpcs[0], simstep_qpcs[-1]
        nf = sum(1 for q in frame_qpcs if lo <= q <= hi)
        fps = nf / (span_ms / 1000.0)
    return {
        "n": n,
        "mean_ms": mean_all,
        "median_ms": median,
        "rate_full": rate_full,
        "deficit_s": deficit_s,
        "slow_frac": slow_frac,
        "fps": fps,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--steps", type=int, default=800)
    ap.add_argument("--base-ms", type=float, default=100.0)
    ap.add_argument(
        "--eps",
        default="0,0.1,0.25,0.5,0.75,1.0",
        help="epsilon list added to --base-ms (fractional ms)",
    )
    ap.add_argument("--out", default=None)
    ap.add_argument("--start-at", type=int, default=0)
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--run-timeout", type=int, default=480)
    ap.add_argument("--host-dir", default=machine.POLYGON)
    ap.add_argument("--host-ip", default=machine.HOST_IP)
    ap.add_argument("--vm-ip", default=machine.RIG_PEER_A)
    ap.add_argument("--vm-user", default=machine.VM_USER)
    ap.add_argument("--vm-dir", default=machine.VM_DIR)
    ap.add_argument("--ssh-key", default=machine.SSH_KEY)
    args = ap.parse_args()

    eps_list = [float(x) for x in args.eps.split(",")]
    steps_ms = [round(args.base_ms + e, 6) for e in eps_list]

    out_path = args.out or os.path.join(REPO, "tmp", "mp_step_sweep_%d.csv" % int(time.time()))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    sweep_archive = os.path.join(args.host_dir, "logs", "step_sweep_%d" % int(time.time()))
    os.makedirs(sweep_archive, exist_ok=True)

    # FPSLimit UNSET on both peers for the whole sweep (design: don't let the fps-cap fix confound).
    host_conf = os.path.join(args.host_dir, "dgVoodoo.conf")
    vm_conf_remote = args.vm_dir.rstrip("\\") + "\\dgVoodoo.conf"
    vm_conf_local_tmp = os.path.join(os.environ.get("TEMP", "."), "mp_step_sweep_vm_dgvoodoo.conf")
    host_conf_backup = host_conf + ".stepsweep.bak"
    shutil.copy2(host_conf, host_conf_backup)
    r = ssh(
        args.ssh_key,
        args.vm_user,
        args.vm_ip,
        "copy /y \"%s\" \"%s.stepsweep.bak\"" % (vm_conf_remote, vm_conf_remote),
    )
    if "cannot find" in (r.stdout + r.stderr).lower():
        raise RuntimeError("VM dgVoodoo.conf not found at %s -- check --vm-dir" % vm_conf_remote)
    print("=== forcing FPSLimit UNSET on host+VM for the whole sweep ===")
    set_fps_limit(host_conf, None)
    shutil.copy2(host_conf, vm_conf_local_tmp)
    scp(
        args.ssh_key,
        vm_conf_local_tmp,
        "%s@%s:%s" % (args.vm_user, args.vm_ip, vm_conf_remote.replace("\\", "/")),
    )

    fieldnames = [
        "idx",
        "eps",
        "step_ms",
        "status",
        "host_rate_full",
        "host_deficit_s",
        "host_mean_ms",
        "host_median_ms",
        "host_slow_frac",
        "host_fps",
        "host_n",
        "client_rate_full",
        "client_deficit_s",
        "client_mean_ms",
        "client_median_ms",
        "client_slow_frac",
        "client_fps",
        "client_n",
        "desync_verdict",
        "desync_first_step",
        "run_dir",
    ]
    write_header = not (args.start_at > 0 and os.path.isfile(out_path))
    csv_f = open(out_path, "a" if not write_header else "w", newline="")
    writer = csv.DictWriter(csv_f, fieldnames=fieldnames)
    if write_header:
        writer.writeheader()
        csv_f.flush()

    print(
        "=== step sweep: %d points, steps=%d, step_ms in %s -> %s ==="
        % (len(steps_ms), args.steps, steps_ms, out_path)
    )
    try:
        for i, step_ms in enumerate(steps_ms):
            if i < args.start_at:
                continue
            if args.limit is not None and (i - args.start_at) >= args.limit:
                print("\n--limit %d reached, stopping" % args.limit)
                break
            eps = eps_list[i]
            print("\n[%d/%d] step_ms=%s (eps=%s)" % (i + 1, len(steps_ms), step_ms, eps))

            clean_peers(args.ssh_key, args.vm_user, args.vm_ip, args.vm_dir)
            row = {"idx": i, "eps": eps, "step_ms": step_ms, "status": "?", "run_dir": ""}
            try:
                cmd = [
                    sys.executable,
                    MP_RUN,
                    "--steps",
                    str(args.steps),
                    "--step-ms",
                    repr(step_ms),
                    "--step-eps-ms",
                    "0",
                    "--sim-step-ms",
                    "0",  # exact grid values + original 100ms sim step (this sweep IS the aliasing probe)
                    "--eager",
                    "1",
                    "--hires",
                    "1",
                    "--qpc",
                    "1",
                    "--temporal",
                    "1",
                    "--host-dir",
                    args.host_dir,
                    "--host-ip",
                    args.host_ip,
                    "--vm-ip",
                    args.vm_ip,
                    "--vm-user",
                    args.vm_user,
                    "--vm-dir",
                    args.vm_dir,
                    "--ssh-key",
                    args.ssh_key,
                ]
                r = sh(cmd, timeout=args.run_timeout)
                if r.returncode != 0:
                    row["status"] = "RUN_FAILED rc=%d" % r.returncode
                    print("    RUN FAILED: %s" % ((r.stderr or r.stdout or "").strip()[-400:]))
                    writer.writerow(row)
                    csv_f.flush()
                    continue

                host_dir = newest_run(os.path.join(args.host_dir, "logs"), "host")
                row["run_dir"] = host_dir or ""
                json_path = os.path.join(host_dir, "mp_analyze.json") if host_dir else None
                desync = {}
                if json_path and os.path.isfile(json_path):
                    import json as _json

                    with open(json_path) as jf:
                        desync = _json.load(jf).get("desync", {})
                row["desync_verdict"] = desync.get("verdict", "")
                row["desync_first_step"] = (desync.get("first_mismatch") or {}).get("step", "")

                host_temporal = os.path.join(host_dir, "mh_temporal.log") if host_dir else None
                scratch = os.path.join(os.environ.get("TEMP", "."), "mp_run")
                client_temporal = os.path.join(scratch, "vm_run", "mh_temporal.log")

                h_ss, h_fr = read_temporal(host_temporal)
                c_ss, c_fr = read_temporal(client_temporal)
                hs = summarize(h_ss, h_fr, step_ms)
                cs = summarize(c_ss, c_fr, step_ms)
                row.update(
                    {
                        "host_rate_full": hs["rate_full"],
                        "host_deficit_s": hs["deficit_s"],
                        "host_mean_ms": hs["mean_ms"],
                        "host_median_ms": hs["median_ms"],
                        "host_slow_frac": hs["slow_frac"],
                        "host_fps": hs["fps"],
                        "host_n": hs["n"],
                        "client_rate_full": cs["rate_full"],
                        "client_deficit_s": cs["deficit_s"],
                        "client_mean_ms": cs["mean_ms"],
                        "client_median_ms": cs["median_ms"],
                        "client_slow_frac": cs["slow_frac"],
                        "client_fps": cs["fps"],
                        "client_n": cs["n"],
                    }
                )
                row["status"] = "OK"

                # archive raw deltas + client scratch so the shape is re-analysable offline
                if host_dir:
                    combo_dir = os.path.join(
                        sweep_archive, "eps_%05.2f_%s" % (eps, os.path.basename(host_dir))
                    )
                    os.makedirs(combo_dir, exist_ok=True)
                    for label, ss in (("host", h_ss), ("client", c_ss)):
                        with open(
                            os.path.join(combo_dir, label + "_simstep_deltas_ms.txt"), "w"
                        ) as df:
                            df.write(
                                "\n".join("%.4f" % ((b - a) / 1000.0) for a, b in zip(ss, ss[1:]))
                            )
                    for n in ("mh_harness.log", "mh_lockstep.log", "mh_net.log", "mh_temporal.log"):
                        src = os.path.join(scratch, "vm_run", n)
                        if os.path.isfile(src):
                            shutil.copy2(src, os.path.join(combo_dir, "client_" + n))
                    if json_path and os.path.isfile(json_path):
                        shutil.copy2(json_path, os.path.join(combo_dir, "mp_analyze.json"))

                def fmt(d):
                    return (
                        "rate=%.4f deficit=%+.2fs mean=%.2fms med=%.2fms slow=%.1f%% fps=%.0f n=%d"
                        % (
                            d["rate_full"] or -1,
                            d["deficit_s"] or 0,
                            d["mean_ms"] or -1,
                            d["median_ms"] or -1,
                            (d["slow_frac"] or 0) * 100,
                            d["fps"] or -1,
                            d["n"],
                        )
                    )

                print("    host   %s" % fmt(hs))
                print("    client %s" % fmt(cs))
                print("    desync: %s" % (row["desync_verdict"] or "(none recorded)"))
            except subprocess.TimeoutExpired:
                row["status"] = "TIMEOUT"
                print("    TIMEOUT after %ds" % args.run_timeout)
            except Exception as e:
                row["status"] = "ERROR: %s" % e
                print("    ERROR: %s" % e)
            finally:
                writer.writerow(row)
                csv_f.flush()
    finally:
        csv_f.close()
        print("\n=== restoring dgVoodoo.conf on host+VM ===")
        shutil.copy2(host_conf_backup, host_conf)
        os.remove(host_conf_backup)
        scp(
            args.ssh_key,
            host_conf,
            "%s@%s:%s" % (args.vm_user, args.vm_ip, vm_conf_remote.replace("\\", "/")),
        )
        ssh(
            args.ssh_key,
            args.vm_user,
            args.vm_ip,
            "del /q \"%s.stepsweep.bak\" 2>nul & echo x" % vm_conf_remote,
        )
        clean_peers(args.ssh_key, args.vm_user, args.vm_ip, args.vm_dir)
        print("results: %s" % out_path)
        print("archive: %s" % sweep_archive)


if __name__ == "__main__":
    main()
