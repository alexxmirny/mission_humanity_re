#!/usr/bin/env python3
"""Run a PAIRED determinism experiment: N runs per arm, artifacts pulled and decoded, one row each.

WHY THIS EXISTS. On 2026-07-29 four near-identical shell drivers were hand-written in a single session
(`runmany.sh`, `d14.sh`, `d14_sat.sh`, `d14_delay.sh`) differing only in arms, flags and output dir --
exactly the duplication the "extend tools/ engines, never hand-roll a one-off" policy exists to
prevent. Two of them also demonstrated hazards that are specific to sh and simply absent here:

  * EDITING A RUNNING SCRIPT CORRUPTS IT. `sh` reads a script incrementally, so adding a variable to a
    driver mid-batch shifted the byte offsets of the loops it had not reached yet; one run reported
    twice, in two different formats, with an empty verdict and a third of its steps. Python compiles
    the whole module before executing any of it. With a parameterised runner you change ARGUMENTS, not
    the script, so the hazard cannot arise at all.
  * `taskkill` EXITS 128 WHEN NOTHING MATCHES, which is the normal case for a pre-run sweep, and under
    `set -e` that kills the driver on its first line.

WHAT IT REPORTS, and the column that matters most: `fired`. A determinism verdict alone is not evidence
about a mechanism that never occurred -- an idle LAN fires ~zero resyncs, so nine consecutive green runs
can say nothing whatsoever about resync-path code (D14). Every row therefore states
whether the tracked order kind actually appeared, and the summary counts vacuous runs separately from
passing ones. A run where the mechanism did not fire is NOT a pass; it is no data.

USAGE
    python tools/rig_batch.py --out tmp/l1p/d14 --runs 4 --steps 3000 \
        --arm off:resync_order_horizon=0 --arm on:resync_order_horizon=1

    # a link-condition arm, and a run that only prints its plan
    python tools/rig_batch.py --out tmp/x --runs 2 --shim-delay 100 --arm base: --dry-run

Each --arm is `name:k=v;k=v` -- the k=v list goes through to `ui_test.py --net-extra`, which OVERRIDES
rather than appends (a duplicate key in a later [net] line is silently ignored by GetPrivateProfile*).
An arm with an empty list (`--arm base:`) runs the shipped defaults.

Writes <out>/SUMMARY.txt (human, one line per run) and <out>/results.json (machine-readable, so the
next question does not have to be answered by grepping a tee'd log -- which is how a corrupted run
went unnoticed until its output format changed).
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

import machine_config as machine  # noqa: E402  (needs the sys.path line above)
import mp_order_diff  # noqa: E402  (needs the sys.path line above)

DEFAULT_KEY = machine.SSH_KEY
SSH_OPTS = ["-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=no"]


def ssh(key, user, ip, cmd, timeout=30):
    """Best-effort ssh. Never raises: every caller here is cleanup or discovery."""
    try:
        return subprocess.run(
            ["ssh"] + SSH_OPTS + ["-i", key, "%s@%s" % (user, ip), cmd],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (subprocess.SubprocessError, OSError):
        return None


def scp(key, src, dst, timeout=120):
    try:
        return (
            subprocess.run(
                ["scp"] + SSH_OPTS + ["-i", key, src, dst], capture_output=True, timeout=timeout
            ).returncode
            == 0
        )
    except (subprocess.SubprocessError, OSError):
        return False


def sweep(a):
    """Kill leftover game processes on EVERY peer, BEFORE each run -- never after.

    A crashed run cannot clean up after itself, which is precisely when cleanup is needed: the next
    run's host then finds the port taken, the client never rendezvouses, and the whole thing sits
    there looking like a slow run for as long as the wall clock allows.
    """
    for ip in a.peers:
        ssh(a.ssh_key, a.vm_user, ip, "taskkill /F /IM mh.focus.exe /T")


def newest_run_dir(a, ip):
    """Newest logs/<run> directory on a peer, for pulling artifacts ui_test does not ferry."""
    r = ssh(
        a.ssh_key,
        a.vm_user,
        ip,
        "for /f %%i in ('dir /b /o-d /ad %s\\logs') do @(echo %%i & exit /b)" % a.vm_dir,
    )
    return r.stdout.strip() if r and r.stdout else ""


def parse_arm(spec):
    """`name:k=v;k=v` -> (name, net_extra, directives). An empty list means the shipped defaults.

    A `@key=value` entry is a RUNNER directive rather than an ini line. Today that is
    `@extra_ini=<path>` / `@extra_ini_host=<path>`, which is what makes an arm able to carry its own ini
    fragment -- and therefore what makes a PAIRED promoted-vs-original batch expressible as one
    invocation instead of two. Two invocations put every run of one arm before every run of the other,
    so any drift in the machine lands entirely on one side; that is not a paired experiment.
    """
    if ":" not in spec:
        raise argparse.ArgumentTypeError(
            "--arm must look like name:k=v;k=v (use `name:` for shipped defaults), got %r" % spec
        )
    name, _, kvs = spec.partition(":")
    if not name:
        raise argparse.ArgumentTypeError("--arm needs a name before the colon: %r" % spec)
    lines, directives = [], {}
    for part in kvs.split(";"):
        part = part.strip()
        if not part:
            continue
        if part.startswith("@"):
            k, _, v = part[1:].partition("=")
            if k not in ("extra_ini", "extra_ini_host"):
                raise argparse.ArgumentTypeError("unknown arm directive @%s" % k)
            directives[k] = v
        else:
            lines.append(part)
    return name.strip(), ";".join(lines), directives


def decode_orders(host_bin, client_bin, kind):
    """Structured form of what mp_order_diff prints. Returns a dict, or None with a reason."""
    if not (os.path.getsize(host_bin) if os.path.isfile(host_bin) else 0):
        return {"error": "no host recording"}
    if not (os.path.getsize(client_bin) if os.path.isfile(client_bin) else 0):
        return {"error": "no client recording"}
    h, c = mp_order_diff.load(host_bin), mp_order_diff.load(client_bin)
    hs, cs = mp_order_diff.by_step(h), mp_order_diff.by_step(c)
    common = sorted(set(hs) & set(cs))
    diffs = [s for s in common if [o.raw for o in hs[s]] != [o.raw for o in cs[s]]]
    out = {"records": len(h), "common_steps": len(common), "differing_steps": len(diffs)}
    if diffs:
        out["first_differing_step"] = diffs[0]
    if kind is not None:
        hk = [o for o in h if o.kind == kind]
        ck = [o for o in c if o.kind == kind]
        out["fired"] = bool(hk) and bool(ck)
        out["fired_host_only"] = bool(hk) != bool(ck)
        if hk:
            out["host_step"], out["exec_time"] = hk[0].step, hk[0].exec_time
        if ck:
            out["client_step"] = ck[0].step
        out["agree"] = bool(
            hk and ck and hk[0].step == ck[0].step and hk[0].exec_time == ck[0].exec_time
        )
    return out


def _digest(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def one_run(a, arm_name, net_extra, idx, directives=None):
    d = os.path.join(a.out, "%s%d" % (arm_name, idx))
    os.makedirs(d, exist_ok=True)
    sweep(a)

    cmd = [
        sys.executable,
        "-u",
        os.path.join(REPO, "tools", "ui_test.py"),
        "--determinism",
        "--steps",
        str(a.steps),
        "--host",
        "%s:%s" % (a.peers[0], a.host_script),
        "--client",
        "%s:%s" % (a.peers[1], a.client_script),
        "--connect-ip",
        a.peers[0],
        "--ship-pacing",
        "--record",
        "1",
        "--timeout",
        str(a.timeout),
        "--stall-timeout",
        str(a.stall_timeout),
    ]
    if net_extra:
        cmd += ["--net-extra", net_extra]
    if a.shim_delay:
        cmd += ["--shim", a.peers[0], "--shim-delay", str(a.shim_delay)]
    directives = directives or {}
    # An arm's own fragment WINS over the batch-wide one, so a paired batch can say "both arms get the
    # video pin, and this arm additionally gets promotion".
    ini = directives.get("extra_ini") or a.extra_ini
    ini_host = directives.get("extra_ini_host") or a.extra_ini_host
    if ini:
        cmd += ["--extra-ini", ini]
    if ini_host:
        cmd += ["--extra-ini-host", ini_host]

    log_path = d + ".log"
    t0 = time.time()
    with open(log_path, "w", encoding="utf-8", errors="replace") as lf:
        try:
            rc = subprocess.run(
                cmd, cwd=REPO, stdout=lf, stderr=subprocess.STDOUT, timeout=a.hard_timeout
            ).returncode
        except subprocess.TimeoutExpired:
            # ui_test's own progress watchdog should fire long before this; reaching it means the
            # RUNNER itself wedged, which is worth distinguishing from a wedged game.
            rc = -9
            lf.write(
                "\n[rig_batch] HARD TIMEOUT after %ds -- the runner itself did not return\n"
                % a.hard_timeout
            )
    elapsed = int(time.time() - t0)

    text = open(log_path, encoding="utf-8", errors="replace").read()
    verdict = next((ln.strip() for ln in text.splitlines() if "VERDICT:" in ln), "(no verdict)")
    stalled = "[det] STALL:" in text

    # Pull the artifacts ui_test does not reliably ferry, straight from each peer's newest run dir.
    for ip, nm in ((a.peers[0], "host_orders.bin"), (a.peers[1], "client_orders.bin")):
        rd = newest_run_dir(a, ip)
        if rd:
            scp(
                a.ssh_key,
                "%s@%s:%s/logs/%s/mh_orders.bin" % (a.vm_user, ip, a.vm_dir.replace("\\", "/"), rd),
                os.path.join(d, nm),
            )
    # Keep the host's own logs. mh_net.log carries the ARMING lines -- an unarmed run and a working run
    # differ only in a line nobody archived. mh_lockstep.log + mh_frametime.log are
    # what mp_pacing_report.py reads, so a pacing comparison can be taken from an archived batch rather
    # than from a hand-rolled loop.
    #
    # AND THE HOST DIR IS STALE-CHECKED, because on 2026-07-29 a hand-rolled loop copied it after a run
    # whose mh.dll deploy had TIMED OUT -- so the previous run's artifacts were archived under the next
    # run's label and read as a data point. Two "different" runs with byte-identical logs is what caught
    # it. Here the guard is structural: if the source file has not changed since the last run archived
    # it, this run gets an explicit marker file instead of a copy.
    host_dir = os.path.join(REPO, "tmp", "ui_test", "determinism", "host")
    for nm in ("mh_net.log", "mh_lockstep.log", "mh_frametime.log"):
        src = os.path.join(host_dir, nm)
        if not os.path.isfile(src):
            continue
        digest = _digest(src)
        if one_run.last_digest.get(nm) == digest:
            with open(os.path.join(d, "host_" + nm + ".STALE"), "w", encoding="utf-8") as f:
                f.write(
                    "The host's %s was byte-identical to the previous run's, i.e. THIS RUN PRODUCED "
                    "NOTHING and what is on disk belongs to the run before it. Not archived, because "
                    "an artifact under the wrong label is worse than a missing one.\n" % nm
                )
            continue
        one_run.last_digest[nm] = digest
        shutil.copy(src, os.path.join(d, "host_" + nm))
        # ALSO under the peer-dir layout the readers expect. mp_pacing_report.py and
        # test_ui.det_run_report both take "a folder holding mh_lockstep.log", i.e.
        # <run>/host/, so archiving only under a host_ PREFIX made every archived batch
        # silently unreadable by them ("no usable in-game mh_lockstep.log" x10).
        os.makedirs(os.path.join(d, "host"), exist_ok=True)
        shutil.copy(src, os.path.join(d, "host", nm))

    row = {
        "arm": arm_name,
        "run": idx,
        "net_extra": net_extra,
        "rc": rc,
        "seconds": elapsed,
        "verdict": verdict,
        "stalled": stalled,
        "clean": "ALL PAIRS IDENTICAL" in verdict,
    }
    if a.armed_marker:
        netlog = os.path.join(d, "host_mh_net.log")
        row["armed"] = (
            a.armed_marker in open(netlog, encoding="utf-8", errors="replace").read()
            if os.path.isfile(netlog)
            else None
        )
    # C5: the run's SHAPE, read from the DLL's machine-readable line rather than inferred. Per-seam
    # gating is a diagnostic facility, so a subset run must never be reported as a ship-config verdict
    # -- and until this was read, a bisect run and a shipping run were distinguishable only by whoever
    # happened to look at the log. Absent = not promoted, which is a legitimate shape (the stock arm).
    netlog = os.path.join(d, "host_mh_net.log")
    row["run_config"] = None
    if os.path.isfile(netlog):
        for ln in open(netlog, encoding="utf-8", errors="replace"):
            k = ln.find("RUN-CONFIG:")
            if k >= 0:
                row["run_config"] = ln[k + len("RUN-CONFIG:") :].strip().split(" ")[0]
                break
    row["orders"] = decode_orders(
        os.path.join(d, "host_orders.bin"), os.path.join(d, "client_orders.bin"), a.kind
    )
    return row


one_run.last_digest = {}


def summarize(rows, kind):
    """Per-arm rollup. Counts VACUOUS runs apart from clean ones -- see the module docstring."""
    out = []
    for arm in dict.fromkeys(r["arm"] for r in rows):
        rs = [r for r in rows if r["arm"] == arm]
        fired = [r for r in rs if (r.get("orders") or {}).get("fired")]
        agree = [r for r in fired if (r.get("orders") or {}).get("agree")]
        out.append(
            "  %-6s %d run(s): %d clean, %d stalled | mechanism fired in %d, of which %d agreed%s"
            % (
                arm,
                len(rs),
                sum(1 for r in rs if r["clean"]),
                sum(1 for r in rs if r["stalled"]),
                len(fired),
                len(agree),
                "" if fired else "  <-- NO DATA: every run was vacuous" if kind is not None else "",
            )
        )
        # C5 enforcement. A DIAGNOSTIC (subset) or REFUSED promotion is a legitimate thing to run --
        # bisecting is what per-seam gating is for -- but its verdict is NOT a ship verdict, and the
        # rollup is where somebody reads a verdict. Say so on the same line they read.
        shapes = {r.get("run_config") for r in rs if r.get("run_config")}
        bad = sorted(s for s in shapes if s not in ("SHIP",))
        if bad:
            out.append(
                "         <-- NOT A SHIP VERDICT: promotion RUN-CONFIG was %s in this arm. Per-seam "
                "gating is a DIAGNOSTIC facility; only a whole-closure SHIP "
                "run may be read as a ship-config result." % ", ".join(bad)
            )
        elif len(shapes) > 1:
            out.append("         <-- MIXED promotion shapes within one arm: %s" % sorted(shapes))
    return out


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--out", required=True, help="output directory (per-run dirs + SUMMARY.txt + results.json)"
    )
    ap.add_argument(
        "--arm", action="append", required=True, type=parse_arm, help="name:k=v;k=v (repeatable)"
    )
    ap.add_argument("--runs", type=int, default=3, help="runs per arm (default 3)")
    ap.add_argument(
        "--steps", type=int, default=3000, help="in-game steps per run (default 3000, ~105 s)"
    )
    ap.add_argument("--kind", default="0xf0", help="order kind to track, or 'none' (default 0xf0)")
    ap.add_argument(
        "--peers", nargs="+", default=list(machine.RIG_PEERS), help="[0]=host, [1]=client"
    )
    ap.add_argument("--ssh-key", default=DEFAULT_KEY)
    ap.add_argument("--vm-user", default=machine.VM_USER)
    ap.add_argument("--vm-dir", default=machine.VM_DIR)
    ap.add_argument("--host-script", default="mp_host_start.txt")
    ap.add_argument("--client-script", default="mp_client_start.txt")
    ap.add_argument(
        "--shim-delay", type=int, default=0, help="one-way shim delay in ms (0 = no shim)"
    )
    ap.add_argument(
        "--extra-ini",
        help="ini fragment appended to BOTH peers (a SYMMETRIC change -- promotion on both sides, a "
        "shadow batch, a [video] pin). Use --extra-ini-host only for a deliberate peer asymmetry.",
    )
    ap.add_argument("--extra-ini-host", help="ini fragment for the HOST peer only")
    ap.add_argument("--armed-marker", help="log substring proving a fix armed, recorded per run")
    ap.add_argument("--timeout", type=int, default=3120, help="ui_test wall clock per run")
    ap.add_argument(
        "--stall-timeout", type=int, default=120, help="ui_test progress watchdog (0=off)"
    )
    ap.add_argument(
        "--hard-timeout", type=int, default=0, help="kill the RUNNER after N s (0 = steps-derived)"
    )
    ap.add_argument(
        "--pacing",
        action="store_true",
        help="after the rollup, print the mp_pacing_report table over the batch's archived host dirs "
        "(labelled per run, stalled runs named as omitted)",
    )
    ap.add_argument("--dry-run", action="store_true", help="print the plan and exit")
    a = ap.parse_args()

    a.kind = None if str(a.kind).lower() == "none" else int(a.kind, 0)
    if len(a.peers) < 2:
        ap.error("--peers needs at least a host and a client")
    # The runner's own bound sits ABOVE ui_test's, which has the progress watchdog; this only catches a
    # wedged runner. Derived from steps so it scales with the run rather than being a magic number.
    if not a.hard_timeout:
        a.hard_timeout = max(900, 300 + a.steps * 2)

    # INTERLEAVED, not arm-major: round n of every arm before round n+1. A batch that runs all of
    # arm A and then all of arm B lets any drift in the machine (another job, a VM under load) land
    # entirely on one side, which is how a paired comparison quietly becomes a before/after one.
    plan = [(n, kv, dv, i) for i in range(1, a.runs + 1) for (n, kv, dv) in a.arm]
    print(
        "[rig_batch] %d run(s): %d arm(s) x %d, %d steps each (~%ds sim), hard timeout %ds"
        % (len(plan), len(a.arm), a.runs, a.steps, a.steps * 0.02, a.hard_timeout)
    )
    for n, kv, dv, i in plan:
        print(
            "    %s%d  net-extra=%s%s"
            % (n, i, kv or "(shipped defaults)", ("  " + str(dv)) if dv else "")
        )
    if a.dry_run:
        return 0

    os.makedirs(a.out, exist_ok=True)
    rows, sum_path = [], os.path.join(a.out, "SUMMARY.txt")
    open(sum_path, "w", encoding="utf-8").close()
    for n, kv, dv, i in plan:
        row = one_run(a, n, kv, i, dv)
        rows.append(row)
        o = row.get("orders") or {}
        line = "%s%d %ds rc=%s%s | %s | fired=%s agree=%s diff_steps=%s%s" % (
            n,
            i,
            row["seconds"],
            row["rc"],
            "" if row.get("armed") is None else " armed=%d" % bool(row["armed"]),
            row["verdict"],
            o.get("fired"),
            o.get("agree"),
            o.get("differing_steps"),
            "  STALLED" if row["stalled"] else "",
        )
        print(line, flush=True)
        with open(sum_path, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        json.dump(rows, open(os.path.join(a.out, "results.json"), "w", encoding="utf-8"), indent=1)

    sweep(a)
    print("\n=== rollup ===")
    for ln in summarize(rows, a.kind):
        print(ln)
        with open(sum_path, "a", encoding="utf-8") as f:
            f.write(ln + "\n")
    print("--- %s" % sum_path)
    if a.pacing:
        pacing_rollup(a, rows)
    return 0


def pacing_rollup(a, rows):
    """Hand the batch's host dirs to mp_pacing_report, labelled by run.

    A paired batch's whole point is comparing arms, and comparing arms means the metric table -- which
    until now had to be reassembled by hand with a glob, once per batch. Doing it here also keeps the
    LABELS attached: a globbed table is a column of identical `host` rows whose arm you have to infer
    from the path order, which is exactly the kind of inference that produced a fake data point on
    2026-07-29.

    Deliberately a SEPARATE pass over the archived dirs rather than something folded into one_run: a
    run that stalled has no usable in-game log, and the report must simply omit it rather than the
    batch pretending it has a row.
    """

    # A row identifies its run as (arm, run), not as a stored path -- results.json has `dir: None`.
    # one_run builds the directory the same way; keep the two in step.
    def run_name(r):
        return "%s%d" % (r["arm"], r["run"])

    dirs = []
    for r in rows:
        d = os.path.join(a.out, run_name(r), "host")
        if os.path.isfile(os.path.join(d, "mh_lockstep.log")):
            dirs.append((run_name(r), d))
    if not dirs:
        print("\n=== pacing === (no run produced a usable in-game mh_lockstep.log)")
        return
    # flush: the child writes straight to the same fd, so an unflushed parent header lands AFTER the
    # table it is supposed to introduce.
    print("\n=== pacing ===", flush=True)
    skipped = [run_name(r) for r in rows if run_name(r) not in {n for n, _ in dirs}]
    if skipped:
        # Never let a silently shorter table read as full coverage.
        print("    omitted (no in-game log): %s" % ", ".join(skipped), flush=True)
    subprocess.run(
        [
            sys.executable,
            os.path.join(os.path.dirname(os.path.abspath(__file__)), "mp_pacing_report.py"),
        ]
        + [d for _, d in dirs],
        check=False,
    )
    # PROMOTION MAKES TWO OF THOSE COLUMNS BLIND, and blind reads as good. The P4 icon counters are a
    # thunk on the wait-overlay CALL SITE 0x0043f0fc, which lives inside llm_strat_time_tick -- a
    # promoted body -- so the interlock suppresses the write and the counters can never advance. A
    # promoted-vs-original table therefore shows e.g. icon_per_1k 70 -> 0 and invites exactly the wrong
    # conclusion. Found on 2026-07-29 in the L1-P step sweep; say it wherever the table is printed.
    promoted = sorted(run_name(r) for r in rows if r.get("run_config"))
    if promoted:
        print(
            "\n    NOTE: icon_per_1k / icon_shown are UNAVAILABLE (not zero) for the promoted run(s) "
            "%s -- the counting thunk sits at 0x0043f0fc, inside the promoted llm_strat_time_tick, so "
            "the site never executes. Do not compare those two columns across arms."
            % ", ".join(promoted)
        )


if __name__ == "__main__":
    sys.exit(main())
