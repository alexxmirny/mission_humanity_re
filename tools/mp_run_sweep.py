#!/usr/bin/env python3
# tools/mp_run_sweep.py -- sweep tools/mp_run.py across (fps_limit x eager x hires x qpc), checking for
# desync + steady-state sim rate per combo (the ep-sweep sibling, 2026-07-20).
#
# Motivation: eager/hires/qpc were only ever A/B'd individually (the MP latency notes) and all came
# back determinism-clean; the one documented desync (Experiment 2, preserve_overshoot) is a DIFFERENT,
# rejected clamp-NOP patch, not any of these three. But the full combination, crossed with a host+client
# dgVoodoo FPSLimit cap, has never been run. This script automates that as a single unattended sweep
# (single host+VM pair -> strictly serial, ~3-4 min/combo).
#
# FPSLimit lives in dgVoodoo.conf on BOTH peers now (host = machine.POLYGON, VM = machine.VM_DIR -- the VM got
# its own dgVoodoo deploy 2026-07-20, superseding the older "VM has no dgVoodoo" note). Backed up and
# restored around the whole sweep; edited only when the fps value changes (fps is the outer loop).
#
# Usage:
#   python tools/mp_run_sweep.py [--steps 800] [--step-ms 100] [--fps none,60,30] [--out tmp/sweep.csv]
#       [--start-at 0]   (resume: skip the first N combos, e.g. after a manual stop)
#       [--host-dir <polygon>] [--vm-ip <vm>] [--vm-user <user>] [--vm-dir <vm-dir>] [--ssh-key <key>]
#   (machine-specific defaults live in tools/machine_config.py)
#
# Per combo: sets FPSLimit (if changed) -> cleans stale peers -> runs mp_run.py -> reads mp_analyze.json
# (desync verdict) + both peers' raw mh_temporal.log (TEV_SIMSTEP id=5 deltas, full-run AND last-N-step
# tail means, since the 2026-07-20 warm-up-transient finding means full-run mean alone is misleading) ->
# archives the client's scratch run into the host run dir (mp_run.py reuses one scratch path per run,
# next combo would overwrite it) -> appends one CSV row -> continues even if a combo run fails/times out.

import argparse, csv, glob, os, re, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import machine_config as machine  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MP_RUN = os.path.join(HERE, "mp_run.py")
TEV_SIMSTEP = 5
TAIL_N = 150  # last-N-step window treated as steady-state (2026-07-20 finding: settles ~60-70s in)


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


def read_simstep_deltas_ms(temporal_log_path):
    ts = []
    if not os.path.isfile(temporal_log_path):
        return ts
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
                ts.append(qpc)
    return [(b - a) / 1000.0 for a, b in zip(ts, ts[1:])]


def summarize(deltas_ms, step_ms):
    if not deltas_ms:
        return {"n": 0, "mean_ms": None, "rate_full": None, "mean_tail_ms": None, "rate_tail": None}
    mean_all = sum(deltas_ms) / len(deltas_ms)
    tail = deltas_ms[-TAIL_N:] if len(deltas_ms) > TAIL_N else deltas_ms
    mean_tail = sum(tail) / len(tail)
    return {
        "n": len(deltas_ms),
        "mean_ms": mean_all,
        "rate_full": step_ms / mean_all,
        "mean_tail_ms": mean_tail,
        "rate_tail": step_ms / mean_tail,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--steps", type=int, default=800)
    ap.add_argument("--step-ms", type=int, default=100)
    ap.add_argument("--fps", default="none,60,30")
    ap.add_argument("--out", default=None)
    ap.add_argument("--start-at", type=int, default=0)
    ap.add_argument(
        "--limit", type=int, default=None, help="stop after this many combos (smoke-testing)"
    )
    ap.add_argument("--run-timeout", type=int, default=480)
    ap.add_argument("--host-dir", default=machine.POLYGON)
    ap.add_argument("--host-ip", default=machine.HOST_IP)
    ap.add_argument("--vm-ip", default=machine.RIG_PEER_A)
    ap.add_argument("--vm-user", default=machine.VM_USER)
    ap.add_argument("--vm-dir", default=machine.VM_DIR)
    ap.add_argument("--ssh-key", default=machine.SSH_KEY)
    args = ap.parse_args()

    fps_values = [None if v.strip().lower() == "none" else int(v) for v in args.fps.split(",")]
    combos = [
        (fps, eager, hires, qpc)
        for fps in fps_values
        for eager in (1, 0)
        for hires in (1, 0)
        for qpc in (1, 0)
    ]

    out_path = args.out or os.path.join(REPO, "tmp", "mp_sweep_%d.csv" % int(time.time()))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    sweep_archive = os.path.join(args.host_dir, "logs", "sweep_%d" % int(time.time()))
    os.makedirs(sweep_archive, exist_ok=True)

    host_conf = os.path.join(args.host_dir, "dgVoodoo.conf")
    vm_conf_local_tmp = os.path.join(os.environ.get("TEMP", "."), "mp_sweep_vm_dgvoodoo.conf")
    vm_conf_remote = args.vm_dir.rstrip("\\") + "\\dgVoodoo.conf"
    host_conf_backup = host_conf + ".sweep.bak"
    shutil.copy2(host_conf, host_conf_backup)
    r = ssh(
        args.ssh_key,
        args.vm_user,
        args.vm_ip,
        "copy /y \"%s\" \"%s.sweep.bak\"" % (vm_conf_remote, vm_conf_remote),
    )
    if "cannot find" in (r.stdout + r.stderr).lower():
        raise RuntimeError("VM dgVoodoo.conf not found at %s -- check --vm-dir" % vm_conf_remote)

    fieldnames = [
        "idx",
        "fps",
        "eager",
        "hires",
        "qpc",
        "status",
        "host_rate_full",
        "host_rate_tail",
        "host_mean_ms",
        "host_mean_tail_ms",
        "host_n",
        "client_rate_full",
        "client_rate_tail",
        "client_mean_ms",
        "client_mean_tail_ms",
        "client_n",
        "desync_verdict",
        "desync_first_step",
        "desync_regions",
        "run_dir",
    ]
    write_header = not (args.start_at > 0 and os.path.isfile(out_path))
    csv_f = open(out_path, "a" if not write_header else "w", newline="")
    writer = csv.DictWriter(csv_f, fieldnames=fieldnames)
    if write_header:
        writer.writeheader()
        csv_f.flush()

    print(
        "=== sweep: %d combos, steps=%d step_ms=%d -> %s ==="
        % (len(combos), args.steps, args.step_ms, out_path)
    )
    last_fps = "unset"
    try:
        for i, (fps, eager, hires, qpc) in enumerate(combos):
            if i < args.start_at:
                continue
            if args.limit is not None and (i - args.start_at) >= args.limit:
                print("\n--limit %d reached, stopping" % args.limit)
                break
            tag = "fps=%s eager=%d hires=%d qpc=%d" % (
                fps if fps is not None else "none",
                eager,
                hires,
                qpc,
            )
            print("\n[%d/%d] %s" % (i + 1, len(combos), tag))

            if fps != last_fps:
                print(
                    "    setting FPSLimit=%s on host+VM ..."
                    % (fps if fps is not None else "(unlimited)")
                )
                set_fps_limit(host_conf, fps)
                # write a local scratch copy with the new value, scp it over the VM's conf
                shutil.copy2(host_conf, vm_conf_local_tmp)  # host+VM confs are identical templates
                scp(
                    args.ssh_key,
                    vm_conf_local_tmp,
                    "%s@%s:%s" % (args.vm_user, args.vm_ip, vm_conf_remote.replace("\\", "/")),
                )
                last_fps = fps

            clean_peers(args.ssh_key, args.vm_user, args.vm_ip, args.vm_dir)

            row = {
                "idx": i,
                "fps": fps if fps is not None else "none",
                "eager": eager,
                "hires": hires,
                "qpc": qpc,
                "status": "?",
                "run_dir": "",
            }
            try:
                cmd = [
                    sys.executable,
                    MP_RUN,
                    "--steps",
                    str(args.steps),
                    "--step-ms",
                    str(args.step_ms),
                    "--eager",
                    str(eager),
                    "--hires",
                    str(hires),
                    "--qpc",
                    str(qpc),
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
                out = r.stdout or ""
                if r.returncode != 0:
                    row["status"] = "RUN_FAILED rc=%d" % r.returncode
                    print("    RUN FAILED: %s" % ((r.stderr or out).strip()[-400:]))
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
                        data = _json.load(jf)
                    desync = data.get("desync", {})
                row["desync_verdict"] = desync.get("verdict", "")
                fm = desync.get("first_mismatch") or {}
                row["desync_first_step"] = fm.get("step", "")
                row["desync_regions"] = ",".join(
                    fm.get("state_only_regions", fm.get("regions", []))
                )

                host_temporal = os.path.join(host_dir, "mh_temporal.log") if host_dir else None
                scratch = os.path.join(os.environ.get("TEMP", "."), "mp_run")
                client_temporal = os.path.join(scratch, "vm_run", "mh_temporal.log")

                hs = summarize(read_simstep_deltas_ms(host_temporal), args.step_ms)
                cs = summarize(read_simstep_deltas_ms(client_temporal), args.step_ms)
                row.update(
                    {
                        "host_rate_full": hs["rate_full"],
                        "host_rate_tail": hs["rate_tail"],
                        "host_mean_ms": hs["mean_ms"],
                        "host_mean_tail_ms": hs["mean_tail_ms"],
                        "host_n": hs["n"],
                        "client_rate_full": cs["rate_full"],
                        "client_rate_tail": cs["rate_tail"],
                        "client_mean_ms": cs["mean_ms"],
                        "client_mean_tail_ms": cs["mean_tail_ms"],
                        "client_n": cs["n"],
                    }
                )
                row["status"] = "OK"

                # archive the client scratch run (mp_run.py reuses one scratch path -> next combo overwrites it)
                if host_dir:
                    combo_dir = os.path.join(
                        sweep_archive, "combo_%02d_%s" % (i, os.path.basename(host_dir))
                    )
                    os.makedirs(combo_dir, exist_ok=True)
                    for n in ("mh_harness.log", "mh_lockstep.log", "mh_net.log", "mh_temporal.log"):
                        src = os.path.join(scratch, "vm_run", n)
                        if os.path.isfile(src):
                            shutil.copy2(src, os.path.join(combo_dir, "client_" + n))
                    if json_path and os.path.isfile(json_path):
                        shutil.copy2(json_path, os.path.join(combo_dir, "mp_analyze.json"))

                print(
                    "    host  rate_full=%.4f rate_tail=%.4f (n=%d)"
                    % (hs["rate_full"] or -1, hs["rate_tail"] or -1, hs["n"])
                )
                print(
                    "    client rate_full=%.4f rate_tail=%.4f (n=%d)"
                    % (cs["rate_full"] or -1, cs["rate_tail"] or -1, cs["n"])
                )
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
            "del /q \"%s.sweep.bak\" 2>nul & echo x" % vm_conf_remote,
        )
        clean_peers(args.ssh_key, args.vm_user, args.vm_ip, args.vm_dir)
        print("results: %s" % out_path)
        print("archive: %s" % sweep_archive)


if __name__ == "__main__":
    main()
