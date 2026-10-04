#!/usr/bin/env python3
"""rig_phases.py -- tooling:TL-RIG-PHASES: per-phase wall time of a rig arm (det_arms / u64_arms -> ui_test.py).

One arm = one `[phase] <name> <seconds>` line per phase + a `[phase] TOTAL` summary, and the same numbers as JSON
(`<det_dir>/rig_phases.json`, plus one appended line in tmp/rig_phases/history.jsonl for the gate to pick up).
MEASUREMENT ONLY: nothing here changes what a run does (the menu walk is never skipped).

HOW THE MARKS ARRIVE. ui_test.py is a child process, so it appends `<name>\\t<epoch>` lines to the file named by
$MH_RIG_PHASE_FILE (`mark()`; a no-op when the variable is unset, first occurrence per name wins). The parent
(`det_arms.run_ui_test` wrapper) sets the variable, runs the child, and folds the marks into the active arm.
The parent adds `arm_begin` (begin(): before lane provisioning) and `arm_end` (end(): after the checker verdict).

PHASES, sequential, each ending at its boundary mark (a missing mark collapses the phase to 0 s, so the phases
always sum to the arm's wall by construction; the independent cross-check is `ui_test_s`, the child's own wall):
  provision   arm_begin    -> exec_host        lane build / VM reachability / clear / ui_test startup / host deploy
  boot        exec_host    -> host_launched    host process start until its run dir (local: past the pack load)
  menu_walk   host_launched-> host_listening   the host's real menu walk until it is hosting in the lobby
  clients     host_listening-> clients_launched  client deploy + process start (all clients)
  lobby       clients_launched-> match_begin   client menu walks, seating, rig signals, until the host's first step
  match       match_begin  -> match_end        the in-game steps (poll granularity: ~8 s at each end)
  pull        match_end    -> pull_end         log pull-back, clean-close wait, peer kill
  analyze     pull_end     -> arm_end          mp_analyze + the arm's checker/report
"""

import json
import os
import time

ENV = "MH_RIG_PHASE_FILE"
# tooling:TL-RIG-PARALLEL -- the per-ARM scratch dir. Unset (the serial default) it is tmp/ui_test; tools/rig_parallel.py
# sets it per slot so concurrent arms never share tmp/ui_test/determinism, the postmortem/red copies or the poll dirs.
SCRATCH_ENV = "MH_RIG_SCRATCH"
BOUNDS = (
    ("provision", "exec_host"),
    ("boot", "host_launched"),
    ("menu_walk", "host_listening"),
    ("clients", "clients_launched"),
    ("lobby", "match_begin"),
    ("match", "match_end"),
    ("pull", "pull_end"),
)
_seen = set()
_arm = None  # {"label", "t0", "marks": {name: t}, "ui_test_s": float}


def mark(name):
    """Child side: record `name` once per process in $MH_RIG_PHASE_FILE (no-op without it)."""
    path = os.environ.get(ENV)
    if not path or name in _seen:
        return
    _seen.add(name)
    try:
        with open(path, "a", encoding="utf-8") as f:
            f.write("%s\t%.3f\n" % (name, time.time()))
    except OSError:
        pass


def _repo():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def scratch_dir():
    """The arm's scratch root: $MH_RIG_SCRATCH when a parallel slot set it, else tmp/ui_test."""
    d = os.environ.get(SCRATCH_ENV) or os.path.join(_repo(), "tmp", "ui_test")
    os.makedirs(d, exist_ok=True)
    return d


def det_dir_path():
    """Where an arm's pulled logs land (ui_test.py writes the same path): <scratch>/determinism."""
    return os.path.join(scratch_dir(), "determinism")


def begin(label):
    """Parent side: start an arm (ends a still-active one first)."""
    global _arm
    if _arm and (_arm["marks"] or _arm["ui_test_s"]):
        end()
    _arm = {"label": label, "t0": time.time(), "marks": {}, "ui_test_s": 0.0}


def run_wrapped(real, argv, timeout, *a, **kw):
    """Call `real(argv, timeout, ...)` (run_ui_test) with the mark file armed; fold its marks into the arm."""
    if _arm is None:
        begin("arm")
    d = os.path.join(_repo(), "tmp", "rig_phases")
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "marks.%d.txt" % os.getpid())
    try:
        os.remove(path)
    except OSError:
        pass
    old = os.environ.get(ENV)
    os.environ[ENV] = path
    t = time.time()
    try:
        return real(argv, timeout, *a, **kw)
    finally:
        if old is None:
            os.environ.pop(ENV, None)
        else:
            os.environ[ENV] = old
        _arm["ui_test_s"] += time.time() - t
        try:
            with open(path, encoding="utf-8") as f:
                for ln in f:
                    k, _, v = ln.strip().partition("\t")
                    if k and v:
                        _arm["marks"].setdefault(k, float(v))
        except OSError:
            pass


def compute(t0, t_end, marks):
    """-> [(phase, seconds)] summing to t_end - t0 (monotonic clamp; a missing mark = 0 s)."""
    cur, out = t0, []
    for phase, key in BOUNDS:
        t = marks.get(key)
        t = cur if t is None else min(max(t, cur), t_end)
        out.append((phase, t - cur))
        cur = t
    out.append(("analyze", t_end - cur))
    return out


def end(det_dir=None):
    """Parent side: close the active arm; print the `[phase]` lines + summary and write the JSON."""
    global _arm
    arm, _arm = _arm, None
    if not arm:
        return None
    t_end = time.time()
    wall = t_end - arm["t0"]
    phases = compute(arm["t0"], t_end, arm["marks"])
    total = sum(s for _, s in phases)
    print("[phase] arm %s" % arm["label"])
    for name, s in phases:
        print("[phase] %-10s %7.1f" % (name, s))
    print(
        "[phase] TOTAL      %7.1f  (arm wall %.1f s; ui_test child %.1f s; phases %.1f%% of wall)"
        % (total, wall, arm["ui_test_s"], 100.0 * total / wall if wall else 100.0)
    )
    events = {
        k: round(v - arm["t0"], 1) for k, v in sorted(arm["marks"].items(), key=lambda kv: kv[1])
    }
    rec = {
        "label": arm["label"],
        "started": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(arm["t0"])),
        "wall_s": round(wall, 1),
        "ui_test_s": round(arm["ui_test_s"], 1),
        "phases": {n: round(s, 1) for n, s in phases},
        "events_s_from_arm_begin": events,
        "missing_marks": [k for _, k in BOUNDS if k not in arm["marks"]],
    }
    try:
        dd = det_dir or det_dir_path()
        if os.path.isdir(dd):
            with open(os.path.join(dd, "rig_phases.json"), "w", encoding="utf-8") as f:
                json.dump(rec, f, indent=1)
        hd = os.path.join(_repo(), "tmp", "rig_phases")
        os.makedirs(hd, exist_ok=True)
        with open(os.path.join(hd, "history.jsonl"), "a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + "\n")
    except OSError as e:
        print("[phase] could not write the JSON: %s" % e)
    return rec


if __name__ == "__main__":
    t0 = 100.0
    r = compute(
        t0,
        200.0,
        {
            "exec_host": 110,
            "host_launched": 120,
            "match_begin": 150,
            "match_end": 190,
            "pull_end": 195,
        },
    )
    assert (
        abs(sum(s for _, s in r) - 100.0) < 1e-9
        and dict(r)["match"] == 40
        and dict(r)["clients"] == 0
    ), r
    print("rig_phases selftest OK")
