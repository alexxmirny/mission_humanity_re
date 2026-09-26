#!/usr/bin/env python3
"""UI regression SUITE -- walk every committed UI test and report pass/fail.

Thin driver over tools/ui_test.py: each entry in TESTS is one canonical scenario (a solo local walk or a
multi-peer VM topology) with a committed baseline under tools/uiscripts/baselines/<script-label>/. This
runs them all (or a filtered subset) and aggregates PASS/FAIL/SKIP so a regression is caught in one command.

    python tools/test_ui.py                 # run every test (solo + VM)
    python tools/test_ui.py --solo          # only the single-peer tests (host-only on the first VM)
    python tools/test_ui.py --only s7_occ_cap match_launch
    python tools/test_ui.py --list          # list tests + exit
    python tools/test_ui.py --update-baselines   # regenerate every baseline (careful)

The other modes are sibling scripts (tooling:TL-SUITE-SPLIT): det_arms.py (--determinism + its
shapes, --sp-determinism), ui_abc.py (--ui-*), tact_test.py (--tact-*), soak_test.py (the all-AI
soak), all over ui_suite_common.py. Their old test_ui.py flags forward there for one cycle.

THE SUITE RUNS LOCALLY BY DEFAULT (since 2026-07-29): one lane folder per peer per test on this box,
headless, `--jobs N` to run tests concurrently. Two reasons that is the default rather than an option --
each test is isolated from the setup.dat / [video] state the previous one left behind, and the peer VMs
stay FREE, so the UI suite and a 2-machine determinism run no longer contend for the same two machines.

Pass --no-local to run across the real VMs instead (the IPs come from --vms, which is the IP list, not
the topology switch). Do that when the thing under test is genuinely machine-dependent -- a cross-machine
transport or timing question. A pure UI/render regression does not need it, and pays VM latency plus
contention with whatever rig run is in flight. With --no-local, multi-peer tests place the host on the
first --vms IP and each client on the next, and a test is SKIPPED (not failed) when a VM is unreachable.
"""

import argparse
import concurrent.futures as cf
import contextlib
import glob
import io as _io
import queue
import json
import os
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lane_alloc  # noqa: E402  fork F4H: the ONE place a lane NUMBER comes from
import ui_registry  # noqa: E402  TL-SUITE-REGDATA: the scenario registries (registry.yaml)
import make_lane  # noqa: E402  LANE_ROOT + the lane builder used by --local
import load_red_allow  # noqa: E402  tooling:TL-SUITE-LOADRED -- the solo-rerun allow-list
import ui_suite_common  # noqa: E402  tooling:TL-HARN19 -- walk_watchdog_selftest drives _run_watched directly
from tact_test import (  # noqa: E402
    run_tact_equiv,
)
from ui_suite_common import (  # noqa: E402
    LOCAL_HOST2_PORT_BASE,
    LOCAL_PORT_BASE,
    LOCAL_SHIM_PORT_BASE,
    LOCAL_TIMEOUT_FRAMES,
    RELAY_PORT,
    REPO,
    WALK_BACKSTOP_MULT,  # tooling:TL-HARN19 -- ui_test.py's own --timeout, raised for --local runs
    WALK_STALL_S,
    RelayProc,
    RunnerConfig,
    TACT_SCENARIOS,
    TESTS,
    add_extra_ini_arg,
    add_net_args,
    add_runner_args,
    apply_net_args,
    build_scenario_argv,
    client_lane_slot,
    client_lane_slots,
    print_desktop_banner,
    provision_lanes,
    relay_addr_for_peers,
    run_ui_test,
    run_ui_test_watched,  # tooling:TL-HARN19 -- the progress-watchdog sibling of run_ui_test
    share_targets,
    sp_newest_run,
    sp_newest_session_run,
    vm_reachable,
)
import importlib  # noqa: E402
import types  # noqa: E402
import tact_test  # noqa: E402  the tactical tail

# ---- TIMEOUT CLASSIFICATION (tooling:TL-SUITE-TIMEOUT-CLASS) ------------------------------------
# A timeout used to read as a bare FAIL whether the scenario never booted, walked to its end and
# then hung before its assertion ever ran, or simply ran slow -- three causes needing three
# different next steps (TL-HARN19's u39_diplomacy: killed at its 420s budget, ALL PAIRS IDENTICAL
# solo at 51s -- the post_check that would have proven that never got to run). Classified from the
# run's OWN ARTIFACTS (a capture, a harness marker, the walk's own end marker, a post_check start
# marker) so a synthetic directory answers exactly like a real rig run -- no ui_test.py, no rig.
UI_TIMEOUT_MARKERS = (
    "TIMED-OUT",
    "WALL-CLOCK TIMEOUT",
    # tooling:TL-SUITE-VMSESSION: a VM-console-preflight refusal bails before launch -- no capture,
    # no harness marker, same NEVER-STARTED shape as a real timeout, just fast.
    "VM SESSION REFUSED",
)  # ui_test.py's own timeout vocabulary
# The name a completed-but-slow run gets when it is over this multiple of its budget_s -- the same
# multiple run_gate.py's SCENARIO_OVER reds a cost growth on, so "SLOW" and "cost red" agree.
SLOW_OVER_BUDGET = 1.5
# The file post_check() touches in a test's run dir(s) the moment it actually starts the checker
# subprocess -- the one artifact this repo cannot derive from ui_test.py/the DLL, because whether
# the ASSERTION ran is a fact about test_ui.py's own control flow (it is gated on rc == 0), not
# about anything the game process wrote.
POST_CHECK_MARKER = "post_check.started"


# A run that failed within this fraction of its own kill bound almost certainly WAS the kill bound
# firing, not an early script/process failure (an install refusal or a crash exits in seconds, not
# at the wall). Backstops the text check under --jobs 1, where run_ui_test streams the child's
# output live and hands back "" -- ui_test.py's own "TIMED-OUT (no marker)" print never reaches the
# text this classifier can read, so wall-clock proximity is the only signal left in that mode.
NEAR_KILL_BOUND_FRAC = 0.95


def looks_like_timeout(rc, text, secs=None, kill_bound=None):
    """True when a FAIL's own text (or the wrapper's own kill, rc==2, or a run that used almost all
    of its own kill-bound seconds) says TIMEOUT, not a script or process failure -- the two need
    different next steps and must never share one word."""
    if rc == 2:  # run_ui_test's own subprocess.TimeoutExpired path
        return True
    if any(m in (text or "") for m in UI_TIMEOUT_MARKERS):
        return True
    return bool(
        rc != 0 and secs is not None and kill_bound and secs >= NEAR_KILL_BOUND_FRAC * kill_bound
    )


def run_artifacts(run_dir):
    """(captures_present, harness_marker_present, walk_reached_end, post_check_started) read from
    ONE run dir's own files. No ui_test.py/rig knowledge here on purpose: a planted synthetic
    directory (the selftest below) must answer exactly like a real one. `mh_uidrive.log` + its
    "; [script] COMPLETE" marker are ui_test.py's local_script_status vocabulary (same file, same
    string) -- reused rather than reinvented so the two readings can never disagree."""
    if not run_dir or not os.path.isdir(run_dir):
        return False, False, False, False
    captures = bool(glob.glob(os.path.join(run_dir, "capture_*.bmp")))
    post_check_started = os.path.isfile(os.path.join(run_dir, POST_CHECK_MARKER))
    log = os.path.join(run_dir, "mh_uidrive.log")
    if not os.path.isfile(log):
        return captures, False, False, post_check_started
    try:
        with open(log, encoding="utf-8", errors="replace") as fh:
            txt = fh.read()
    except OSError:
        return captures, False, False, post_check_started
    harness_marker = bool(txt.strip())
    walk_reached_end = "; [script] COMPLETE" in txt
    return captures, harness_marker, walk_reached_end, post_check_started


def last_script_step(run_dir):
    """The last `; [script]` line of a run dir's mh_uidrive.log, or None -- names WHERE a
    WALK-STALLED run was waiting (TL-SUITE-TIMEOUT-CLASS)."""
    log = os.path.join(run_dir or "", "mh_uidrive.log")
    try:
        with open(log, encoding="utf-8", errors="replace") as fh:
            lines = [ln.strip() for ln in fh if "; [script]" in ln]
    except OSError:
        return None
    return lines[-1] if lines else None


# The timeout classes (tooling:TL-SUITE-TIMEOUT-CLASS) -> the one-line explanation the summary prints.
TIMEOUT_CLASSES = {
    "NEVER-STARTED": "no capture or harness marker in any lane (the run never booted)",
    "WALK-STALLED": "booted, but the walk never reached its end marker (a step waited forever)",
    "ASSERTION-NOT-RUN": "the walk reached its own end marker, but post_check never started before the kill",
}


def _walk_backstop_timeout(test, suite):
    """ui_test.py's own --timeout for a --local run (tooling:TL-HARN19): the row's flat timeout, or
    (if larger) WALK_BACKSTOP_MULT x its budget_s. This becomes ui_test.py's internal deadline of
    last resort -- run_ui_test_watched's progress kill is what actually enforces "give up", so
    ui_test.py's own flat deadline must not fire first while a lane is still genuinely walking."""
    row_timeout = test.get("timeout") or suite["timeout"]
    budget = test.get("budget_s")
    return max(row_timeout, WALK_BACKSTOP_MULT * budget) if budget else row_timeout


def classify_result(
    rc,
    timed_out,
    secs,
    budget_s,
    captures,
    harness_marker,
    walk_reached_end,
    post_check_started,
):
    """Pure: the class for one scenario run, from facts already read off its own artifacts. Never
    touches a file or the rig itself -- that is run_artifacts' + looks_like_timeout's job -- so this
    is the part a selftest can drive with plain booleans.

    Returns one of PASS / FAIL / SLOW / NEVER-STARTED / WALK-STALLED / ASSERTION-NOT-RUN. The last four only ever
    come out of the TIMED-OUT branch: an ordinary script/process FAIL keeps its ordinary name, so a
    ratchet on "never a bare FAIL for a timeout" cannot be satisfied by quietly renaming every FAIL.
    """
    over_budget = bool(budget_s) and secs is not None and secs > SLOW_OVER_BUDGET * budget_s
    if not timed_out:
        if rc != 0:
            return "FAIL"
        return "SLOW" if over_budget else "PASS"
    # TIMED-OUT from here down: the wall clock (or the wrapper's own kill) decided this run, so the
    # class is about WHERE in the run it stopped, not about pass/fail vocabulary.
    if not captures and not harness_marker:
        return "NEVER-STARTED"
    if not walk_reached_end:
        return "WALK-STALLED"  # booted, then waited on a step that never came (u39's in-gate hang)
    if not post_check_started:
        return "ASSERTION-NOT-RUN"
    return "SLOW"


def classify_selftest():
    """`--classify-selftest`: the pure classifier + the artifact reader, against synthetic run dirs
    (no rig, no ui_test.py). Proves the three TL-SUITE-TIMEOUT-CLASS planted shapes -- a script that
    never boots, one that hangs after the walk, and a real pass that is merely slow against a budget
    set below its measured time -- each produce their own named class."""
    import tempfile

    ok = True

    def _dir(**files):
        d = tempfile.mkdtemp(prefix="tclass_")
        for name, body in files.items():
            with open(os.path.join(d, name), "w", encoding="utf-8") as fh:
                fh.write(body)
        return d

    # (1) never boots: no capture, no mh_uidrive.log at all.
    never = _dir()
    c, h, w, p = run_artifacts(never)
    got = classify_result(1, True, 420.0, 60, c, h, w, p)
    hit = got == "NEVER-STARTED"
    ok = ok and hit
    print("  %-42s %s (%s)" % ("never boots -> NEVER-STARTED", "ok" if hit else "XX", got))

    # (2) hangs after the walk: the script's own log reached COMPLETE, a capture exists, but the
    # process never let post_check start before the wrapper killed it.
    hung = _dir(**{"mh_uidrive.log": "; [script] step 1\n; [script] COMPLETE\n"})
    open(os.path.join(hung, "capture_0001.bmp"), "wb").close()
    c, h, w, p = run_artifacts(hung)
    got = classify_result(1, True, 420.0, 60, c, h, w, p)
    hit = got == "ASSERTION-NOT-RUN"
    ok = ok and hit
    print(
        "  %-42s %s (%s)"
        % ("hangs after the walk -> ASSERTION-NOT-RUN", "ok" if hit else "XX", got)
    )

    # (2b) stalls mid-walk: booted (a capture + uidrive log) but never reached COMPLETE.
    stalled = _dir(**{"mh_uidrive.log": "; [script] step 1\n", "capture_000.bmp": "x"})
    c, h, w, p = run_artifacts(stalled)
    got = classify_result(1, True, 420.0, 60, c, h, w, p)
    hit = got == "WALK-STALLED"
    ok = ok and hit
    print("  %-42s %s (%s)" % ("stalls mid-walk -> WALK-STALLED", "ok" if hit else "XX", got))

    # (3) slow but asserted: NOT a timeout -- it finished (rc 0), post_check ran, and its budget_s
    # was simply set below the real measured time (the third planted shape).
    slow = _dir(**{"mh_uidrive.log": "; [script] step 1\n; [script] COMPLETE\n"})
    open(os.path.join(slow, "capture_0001.bmp"), "wb").close()
    open(os.path.join(slow, POST_CHECK_MARKER), "w").close()
    c, h, w, p = run_artifacts(slow)
    got = classify_result(0, False, 100.0, 60, c, h, w, p)
    hit = got == "SLOW"
    ok = ok and hit
    print("  %-42s %s (%s)" % ("slow but asserted -> SLOW", "ok" if hit else "XX", got))

    # An ordinary process/script FAIL (not a timeout) keeps FAIL -- the ratchet is "never a bare
    # FAIL for a TIMEOUT", not "never say FAIL again".
    got = classify_result(1, False, 10.0, 60, False, True, False, False)
    hit = got == "FAIL"
    ok = ok and hit
    print("  %-42s %s (%s)" % ("ordinary script FAIL stays FAIL", "ok" if hit else "XX", got))

    for d in (never, hung, slow):
        shutil.rmtree(d, ignore_errors=True)
    print("classify-selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def walk_watchdog_selftest():
    """`--walk-watchdog-selftest` (tooling:TL-HARN19): the three offline proofs against a FAKE child
    process (no rig, no ui_test.py) -- run_ui_test_watched must not kill a lane that is still
    genuinely writing however slowly, must kill one that stops writing after ~stall_s (classified
    WALK-STALLED, its last step named), and must kill one that never writes at all within the
    backstop (classified NEVER-STARTED). Small numbers throughout so this runs in a few seconds."""
    import tempfile

    ok = True

    def _child(body):
        return [sys.executable, "-c", body]

    tmp = tempfile.mkdtemp(prefix="walkwd_")

    # (1) SLOW but genuinely progressing: writes a new `; [script]` line every ~0.4s for 6 steps
    # (~2.4s total), well past a hypothetical "old flat bound" of 1s -- must NOT be killed.
    slow_dir = os.path.join(tmp, "slow")
    os.makedirs(slow_dir)
    slow_log = os.path.join(slow_dir, "mh_uidrive.log")
    slow_cmd = _child(
        "import time\n"
        "for i in range(6):\n"
        "    time.sleep(0.4)\n"
        "    open(%r, 'a').write('; [script] %%d ok\\n' %% i)\n" % slow_log
    )
    t0 = time.time()
    rc, _ = ui_suite_common._run_watched(
        slow_cmd, backstop=30, run_dirs_fn=lambda: [slow_dir], stall_s=1.2, capture=True, poll_s=0.2
    )
    secs = time.time() - t0
    hit = (
        rc == 0 and secs > 2.0
    )  # ran the full ~2.4s (over 2x an "old" 1s flat bound) and exited clean
    ok = ok and hit
    print(
        "  %-55s %s (rc=%s, %.1fs)"
        % (
            "slow-but-progressing, past an old 1s bound -> NOT killed",
            "ok" if hit else "XX",
            rc,
            secs,
        )
    )

    # (2) STALLS after 2 steps, then goes quiet for far longer than stall_s -- must be killed at
    # roughly stall_s after its last write, classified WALK-STALLED with that last line named.
    stall_dir = os.path.join(tmp, "stall")
    os.makedirs(stall_dir)
    stall_log = os.path.join(stall_dir, "mh_uidrive.log")
    stall_cmd = _child(
        "import time\n"
        "open(%r, 'a').write('; [script] 0 ok\\n')\n"
        "time.sleep(0.3)\n"
        "open(%r, 'a').write('; [script] 1 ok\\n')\n"
        "time.sleep(20)\n" % (stall_log, stall_log)
    )
    t0 = time.time()
    rc, text = ui_suite_common._run_watched(
        stall_cmd,
        backstop=30,
        run_dirs_fn=lambda: [stall_dir],
        stall_s=1.0,
        capture=True,
        poll_s=0.2,
    )
    secs = time.time() - t0
    cap, harn, walk, pcs = run_artifacts(stall_dir)
    cls = classify_result(rc, True, secs, None, cap, harn, walk, pcs)
    hit = rc == 2 and cls == "WALK-STALLED" and secs < 10 and "advanced" in text
    ok = ok and hit
    print(
        "  %-55s %s (rc=%s, class=%s, %.1fs, last=%r)"
        % (
            "stalls after 2 steps -> killed, WALK-STALLED",
            "ok" if hit else "XX",
            rc,
            cls,
            secs,
            last_script_step(stall_dir),
        )
    )

    # (3) NEVER writes anything -- must be killed by the backstop alone (stall_s never even sees a
    # fingerprint to compare), classified NEVER-STARTED.
    silent_dir = os.path.join(tmp, "silent")
    os.makedirs(silent_dir)
    silent_cmd = _child("import time\ntime.sleep(20)\n")
    t0 = time.time()
    rc, text = ui_suite_common._run_watched(
        silent_cmd,
        backstop=1.5,
        run_dirs_fn=lambda: [silent_dir],
        stall_s=1.0,
        capture=True,
        poll_s=0.2,
    )
    secs = time.time() - t0
    cap, harn, walk, pcs = run_artifacts(silent_dir)
    cls = classify_result(rc, True, secs, None, cap, harn, walk, pcs)
    hit = rc == 2 and cls == "NEVER-STARTED" and "backstop" in text
    ok = ok and hit
    print(
        "  %-55s %s (rc=%s, class=%s, %.1fs)"
        % (
            "never writes -> killed by the backstop, NEVER-STARTED",
            "ok" if hit else "XX",
            rc,
            cls,
            secs,
        )
    )

    shutil.rmtree(tmp, ignore_errors=True)
    print("walk-watchdog-selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# The shim's localhost TCP CONTROL port, per shim row (2026-09-24, tooling gate diet block 1). It
# was ui_test.SHIM_CONTROL_PORT = 6699 for every shim on the box -- a machine-wide singleton -- so
# two shim rows in flight at once and the second net_shim.py died at bind, and the suite folded
# every `"shim": True` row into ONE serial worker (1536 s of a 1731 s suite wall, 2026-09-23 gate).
# Derived from the row's shim port exactly as that is derived from its index (base + ti), in its own
# band; lane_alloc.py --check proves no two registry rows can land on one port across all four bands.
LOCAL_SHIM_CTL_PORT_BASE = 6900


def shim_control_port(shim_port):
    """The control port that goes with a local shim listen port (0 -> 0: no shim, no control)."""
    return (LOCAL_SHIM_CTL_PORT_BASE + (shim_port - LOCAL_SHIM_PORT_BASE)) if shim_port else 0


def row_ports(ti, test):
    """Every local port registry row #ti (0-based, in the run's `tests` order) can bind or dial --
    {role: port}. provision_lanes + shim_argv derive theirs from these same formulas; lane_alloc's
    --check calls this over the whole registry to prove rows are pairwise disjoint."""
    out = {"game": LOCAL_PORT_BASE + ti}
    if test.get("shim"):
        out["shim"] = LOCAL_SHIM_PORT_BASE + ti
        out["shim_ctl"] = shim_control_port(out["shim"])
    if test.get("host_lanes"):
        out["host2"] = LOCAL_HOST2_PORT_BASE + ti
    return out


# D15: a test this close to its wall-clock budget is reported as a NEAR-MISS. 0.7 rather than 0.9
# because the whole point is lead time -- the 2026-08-06 failures were tests that had been sitting
# near their budget while the suite reported them as ordinary passes, so the warning has to fire
# while there is still headroom to act on.
WARN_FRAC = 0.7


# ---- THE ROUTINE: every row carries its expected cost (gate diet block 4a, 2026-09-24) ----------
# `budget_s` is the scenario's EXPECTED wall seconds under gate load -- seeded from a green run's
# measured time x ~1.3, rounded -- NOT its kill bound (that is `timeout`). run_gate.py goes RED when
# a scenario runs longer than SCENARIO_OVER (1.5) x its budget_s, naming the growth against the last
# green run. A row over BUDGET_LONG_S must say why in `long_why`: a long row is a decision, not a
# default. Enforced by `test_ui.py --check-budgets` (a lint_repo row) -- see the ui-testing skill's
# "Growing the regression suite" for how to set or raise one.
BUDGET_LONG_S = 120
# The seed floor: a row that measured a few seconds gets 30, not 10 -- under a loaded gate a boot alone
# can cost 15 s, and 1.5x a 10 s budget would red on machine noise rather than on a row that grew.
BUDGET_FLOOR_S = 30


def _committed_budgets():
    """name -> budget_s as committed at HEAD (tools/uiscripts/registry.yaml), the baseline a live
    budget_s raise is checked against (TL-SUITE-TIMEOUT-CLASS). git is the record, so there is no
    baseline file to go stale. Returns {} on any git/parse failure -- a box with no history must not
    red every row."""
    import ui_registry  # noqa: PLC0415

    try:
        out = subprocess.run(
            ["git", "show", "HEAD:tools/uiscripts/registry.yaml"],
            cwd=REPO,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=15,
        )
        if out.returncode != 0 or not out.stdout:
            return {}
        data = ui_registry.load_text(out.stdout, lint=False)  # HEAD may predate a lint
    except (OSError, subprocess.SubprocessError, ValueError):
        return {}
    return {
        t["name"]: t["budget_s"]
        for t in data.get("tests", [])
        if isinstance(t.get("budget_s"), int)
    }


def budget_problems(tests, baseline=None):
    """Problem strings for the registry's budget discipline; empty = sound. `baseline` (name ->
    committed budget_s, from `_committed_budgets()`) is optional so every existing caller/selftest
    that never mentions it is unaffected -- the raise-without-evidence rule only fires when given
    something to compare against."""
    bad = []
    for t in tests:
        b = t.get("budget_s")
        if b is None:
            bad.append("%s: no budget_s (expected seconds; seed = a green run x ~1.3)" % t["name"])
            continue
        if not isinstance(b, int) or b <= 0:
            bad.append("%s: budget_s must be a positive int, got %r" % (t["name"], b))
            continue
        if b > BUDGET_LONG_S and not (t.get("long_why") or "").strip():
            bad.append(
                "%s: budget_s %d > %d with no long_why -- say why this row needs it"
                % (t["name"], b, BUDGET_LONG_S)
            )
        if t.get("timeout") and b > t["timeout"]:
            bad.append(
                "%s: budget_s %d exceeds its own kill bound timeout %d"
                % (t["name"], b, t["timeout"])
            )
        if baseline:
            old = baseline.get(t.get("name"))
            if old is not None and b > old and not (t.get("budget_evidence") or "").strip():
                bad.append(
                    "%s: budget_s raised %d -> %d with no budget_evidence (a log path or run id "
                    "proving the growth is real, not the number alone)" % (t["name"], old, b)
                )
    return bad


def check_budgets():
    """`--check-budgets`: the lint row. Runs its own negative cases first, then the live registry."""
    cases = [
        ("a row with no budget_s", [{"name": "a"}], "no budget_s", None),
        ("budget_s > 120 without long_why", [{"name": "a", "budget_s": 200}], "no long_why", None),
        ("a non-int budget", [{"name": "a", "budget_s": "60"}], "positive int", None),
        (
            "budget over its own timeout",
            [{"name": "a", "budget_s": 90, "timeout": 60}],
            "kill bound",
            None,
        ),
        (
            "a budget raised with no evidence",
            [{"name": "a", "budget_s": 90}],
            "budget_evidence",
            {"a": 60},
        ),
    ]
    ok = True
    for label, rows, needle, base in cases:
        hit = any(needle in b for b in budget_problems(rows, baseline=base))
        ok = ok and hit
        print("  %-36s %s" % (label, "CAUGHT" if hit else "MISSED"))
    clean = budget_problems([{"name": "a", "budget_s": 200, "long_why": "measured 150 s"}])
    print("  %-36s %s" % ("a justified long row passes", "ok" if not clean else "XX"))
    ok = ok and not clean
    clean_raise = budget_problems(
        [{"name": "a", "budget_s": 90, "budget_evidence": "tmp/gate/logs/suite.log"}],
        baseline={"a": 60},
    )
    print("  %-36s %s" % ("a raise WITH evidence passes", "ok" if not clean_raise else "XX"))
    ok = ok and not clean_raise
    live = budget_problems(TESTS, baseline=_committed_budgets())
    for b in live:
        print("  " + b)
    n_long = sum(1 for t in TESTS if (t.get("budget_s") or 0) > BUDGET_LONG_S)
    print(
        "check-budgets: %s -- %d rows, %d over %d s (each with a long_why), sum %d s"
        % (
            "PASS" if ok and not live else "FAIL",
            len(TESTS),
            n_long,
            BUDGET_LONG_S,
            sum(t.get("budget_s") or 0 for t in TESTS),
        )
    )
    return 0 if ok and not live else 1


# ---- tooling:TL-SUITE-LOADRED -- solo rerun of red rows ----------------------------------------
# TL-HARN19 measured u39_diplomacy at 51s/420s alone vs 424s (killed) inside the full suite, ALL
# PAIRS IDENTICAL never getting to run; TL-GATE-LOADFLAKE-0925 named five more reds the same
# afternoon, every one of them clean standalone. A verdict word (FAIL/NEVER-STARTED/...) cannot tell
# "the code is wrong" from "seven other lanes were on the box" apart -- so re-run each red ALONE,
# once, after every other row has already finished (the pool is idle here by construction, no --jobs
# override needed) and report which one it was.
RED_VERDICTS = ("FAIL", *TIMEOUT_CLASSES)
# Bounds gate cost (memory: gate-cost-is-a-verdict) -- a suite that is already red is already the
# slow path, and re-running every red row would double an already-blown budget on the run that can
# least afford it. 3 covers every incident measured so far (TL-GATE-LOADFLAKE-0925 named 5 reds in
# one gate, but never more than 3 that were not EXPECTED to be related, e.g. a shared-cause install
# refusal that SKIPs the rest instead of reaching this code at all).
LOAD_RERUN_CAP = 3


def apply_load_rerun(tests, results, timings, run_one, cap=LOAD_RERUN_CAP, allow_check=None):
    """Mutates `results`/`timings` in place for every non-`expect_red` row whose verdict is in
    RED_VERDICTS; returns the {name: {...}} detail dict for the suite's own record. Pure enough to
    selftest: `run_one(t)` is handed the just-failed test dict and must run it ALONE, returning
    (verdict, secs) -- the caller (main()) wires that to the real run_one; a selftest wires it to a
    canned answer and needs no rig. `allow_check(name)` defaults to load_red_allow.allowed.

    Verdicts written into `results`: LOAD-RED-ALLOW (passed alone, on the allow-list -- counts as a
    pass), LOAD-RED (passed alone, NOT allow-listed -- still fails), RED-NOT-RERUN (over the cap --
    still fails), or the ORIGINAL verdict is left untouched (failed again alone -- a real red, not a
    load artifact, so its own specific word -- FAIL/NEVER-STARTED/ASSERTION-NOT-RUN -- keeps meaning
    what it always meant)."""
    allow_check = allow_check or load_red_allow.allowed
    reds = [t for t in tests if results.get(t["name"]) in RED_VERDICTS and not t.get("expect_red")]
    rerun_now, over_cap = reds[:cap], reds[cap:]
    info = {}
    for t in over_cap:
        info[t["name"]] = {
            "first_verdict": results[t["name"]],
            "first_secs": round(timings.get(t["name"], (0.0, 0))[0], 1),
            "rerun_verdict": None,
            "rerun_secs": None,
            "outcome": "RED-NOT-RERUN",
            "allow_reason": "",
        }
        results[t["name"]] = "RED-NOT-RERUN"
    for t in rerun_now:
        name = t["name"]
        first_verdict = results[name]
        first_secs = timings.get(name, (0.0, 0))[0]
        verdict2, secs2 = run_one(t)
        passed_alone = verdict2 in ("PASS", "SLOW")
        allow, reason = allow_check(name) if passed_alone else (False, "")
        outcome = "RED" if not passed_alone else ("LOAD-RED-ALLOW" if allow else "LOAD-RED")
        info[name] = {
            "first_verdict": first_verdict,
            "first_secs": round(first_secs, 1),
            "rerun_verdict": verdict2,
            "rerun_secs": round(secs2, 1),
            "outcome": outcome,
            "allow_reason": reason,
        }
        if outcome != "RED":
            results[name] = outcome
        # The summary's budget column reads the FIRST run's timing -- the rerun's own elapsed rides
        # in `info` instead, so a load-flaky row's normal cost is not overwritten by its own retry.
        timings[name] = (first_secs, timings.get(name, (0.0, 0))[1])
    return info


def loadred_selftest():
    """`--loadred-selftest`: apply_load_rerun against a fake run_one (no rig, no suite). Proves a
    pass-alone row is reported LOAD-RED (or LOAD-RED-ALLOW when the allow-list says so), a row that
    fails again alone stays RED under its ORIGINAL verdict word, an expect_red row is never rerun at
    all, and the cap reports the overflow as RED-NOT-RERUN without calling run_one for it."""
    ok = True

    def make(name, verdict, expect_red=None):
        t = {"name": name, "kind": "solo", "desc": "d", "budget_s": 10}
        if expect_red:
            t["expect_red"] = expect_red
        return t

    def check(label, cond):
        nonlocal ok
        print("  %s  %s" % ("ok  " if cond else "FAIL", label))
        ok = ok and cond

    # Case 1: a mixed batch under the cap -- one passes alone (load-flaky), one fails again (real
    # red), one carries expect_red (must be skipped by the rerun entirely).
    tests = [
        make("a_flaky", "FAIL"),
        make("b_real", "NEVER-STARTED"),
        make("c_xfail", "FAIL", "T1"),
    ]
    results = {"a_flaky": "FAIL", "b_real": "NEVER-STARTED", "c_xfail": "XFAIL"}
    timings = {"a_flaky": (400.0, 420), "b_real": (410.0, 420)}
    calls = []

    def fake_run_one(t):
        calls.append(t["name"])
        return {"a_flaky": ("PASS", 55.0), "b_real": ("NEVER-STARTED", 415.0)}[t["name"]]

    info = apply_load_rerun(
        tests, results, timings, fake_run_one, cap=3, allow_check=lambda n: (False, "")
    )
    check("a pass-alone row is reported LOAD-RED", results["a_flaky"] == "LOAD-RED")
    check(
        "LOAD-RED keeps the FIRST run's timing in the summary column",
        timings["a_flaky"] == (400.0, 420),
    )
    check(
        "its rerun timing rides in the detail dict instead", info["a_flaky"]["rerun_secs"] == 55.0
    )
    check(
        "a row that fails again alone keeps its ORIGINAL verdict word",
        results["b_real"] == "NEVER-STARTED" and info["b_real"]["outcome"] == "RED",
    )
    check("an expect_red row is never rerun", "c_xfail" not in calls and "c_xfail" not in info)

    # Case 2: the SAME pass-alone row, now on the allow-list -> LOAD-RED-ALLOW (a pass).
    results2 = {"a_flaky": "FAIL"}
    timings2 = {"a_flaky": (400.0, 420)}
    info2 = apply_load_rerun(
        [make("a_flaky", "FAIL")],
        results2,
        timings2,
        fake_run_one,
        cap=3,
        allow_check=lambda n: (True, "TL-FAKE (expires 2099-01-01)"),
    )
    check(
        "an allow-listed pass-alone row is LOAD-RED-ALLOW, not LOAD-RED",
        results2["a_flaky"] == "LOAD-RED-ALLOW" and "TL-FAKE" in info2["a_flaky"]["allow_reason"],
    )

    # Case 3: the cap. 4 reds at cap=3 -> only the first 3 are ever handed to run_one; the 4th is
    # RED-NOT-RERUN and run_one is never called for it (verifies the cap bounds COST, not just count).
    names = ["r1", "r2", "r3", "r4"]
    tests3 = [make(n, "FAIL") for n in names]
    results3 = {n: "FAIL" for n in names}
    timings3 = {n: (100.0, 200) for n in names}
    calls3 = []

    def fake_run_one3(t):
        calls3.append(t["name"])
        return "PASS", 10.0

    info3 = apply_load_rerun(
        tests3, results3, timings3, fake_run_one3, cap=3, allow_check=lambda n: (False, "")
    )
    check("the cap reruns exactly `cap` rows", sorted(calls3) == ["r1", "r2", "r3"])
    check(
        "the row over the cap is RED-NOT-RERUN, never handed to run_one",
        results3["r4"] == "RED-NOT-RERUN"
        and "r4" not in calls3
        and info3["r4"]["rerun_verdict"] is None,
    )

    print("loadred selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def shim_spec(test, forward_to, shim_port=0):
    """build_scenario_argv's `shim` for a link-condition row (None when it has none): the peers run
    through tools/net_shim.py, whose lifetime ui_test owns. Locally the shim needs its own listen and
    control ports (H3, concurrent shim rows); triggers are evidence-bounded actions (gate diet 2)."""
    if not test.get("shim"):
        return None
    spec = {
        "target": forward_to,
        "delay": test.get("shim_delay", 0),
        "timeline": test.get("shim_timeline"),
        "triggers": test.get("shim_triggers") or [],
    }
    if shim_port:
        spec["listen_port"] = shim_port
        spec["control_port"] = shim_control_port(shim_port)
    return spec


def build_argv(test, vms, suite, plan=None, cfg=None, local=False):
    """One registry row's ui_test argv. `suite` = the run-wide options main() resolved; a row's own
    value wins over the suite's (--tol / --timeout / --timeout-frames / --net-extra).

    `local=True` (tooling:TL-HARN19) raises the `--timeout` ui_test.py itself receives to
    `_walk_backstop_timeout` instead of the row's flat value -- the caller's run_ui_test_watched is
    what actually enforces "give up" for a --local run, from outside, on progress rather than a flat
    deadline, so ui_test.py's own internal deadline must not fire first. VM/--no-local runs are
    unaffected: they still use run_ui_test's flat kill, so ui_test.py's own timeout should stay the
    row's real number."""
    ei = test.get("extra_ini")
    ei = [ei] if isinstance(ei, str) else list(ei or [])
    # Every multi row gets the [video] fps_cap (peers pace on each other, not the CPU);
    # `fps_cap: False` opts out. Solo rows stay uncapped: menus advance per present.
    if test.get("kind") == "multi" and test.get("fps_cap", True):
        ei.append("tools/uiscripts/ini/fps60.ini")
    if suite.get("extra_ini_all"):
        ei.append(suite["extra_ini_all"])
    # mp:P13 -- ship pacing on a headless lane needs the fps cap standing in for vsync.
    if test.get("ship_pacing") and (test.get("kind") != "multi" or not test.get("fps_cap", True)):
        raise SystemExit("%s: ship_pacing needs a capped multi row" % test["name"])
    opts = dict(
        extra_ini=ei,
        extra_ini_host=test.get("extra_ini_host"),  # mp:F3c -- per-side fragments
        extra_ini_client=test.get("extra_ini_client"),
        timeout=_walk_backstop_timeout(test, suite)
        if local
        else (test.get("timeout") or suite["timeout"]),
        tol=test["tol"] if test.get("tol") is not None else suite["tol"],
        pixdelta=suite["pixdelta"],
        update_baselines=suite["update_baselines"],
        # ui_test's --net-extra is ONE value: a row's own net_extra replaces the suite's
        net_extra=test.get("net_extra") or suite["net_extra"],
        headless=suite["headless"],
        timeout_frames=test.get("timeout_frames") or suite["timeout_frames"],
        launch_args=test.get("launch_args"),
        deploy_save=test.get("deploy_save"),
        harness_extra=test.get("harness_extra"),
        harness_extra_host=test.get("harness_extra_host"),  # mp:X1b -- deliberately asymmetric
        net_extra_client=test.get("net_extra_client"),  # mp:R7a
        ship_pacing=bool(test.get("ship_pacing")),
        force_headless=bool(test.get("ship_pacing")),
        no_client_ip=bool(test.get("no_client_ip")),  # mp:R7a
        client_game_name=test.get("client_game_name"),  # mp:R2b
        signal_touch=test.get("signal_touch"),  # mp:R4b -- set by the runner
        client_after_exit=list((test.get("client_after_exit") or {}).items()),  # mp:GS1(b)
        client_expect_exit=list(test.get("client_expect_exit") or []),
    )

    def client_idx(i):
        # mp:GS1(b): a reused client resolves to the SAME lane/VM as the peer it reuses
        return 1 + client_lane_slots(test).index(client_lane_slot(test, i + 1))

    if plan is not None:  # --local: every peer of every test is its own lane on this machine
        port, shim_port, names = plan[test["name"]]
        if test["kind"] == "solo":
            # --port even solo: ui_test's readiness gate polls the lane's own port
            return build_scenario_argv(
                cfg=cfg, host="lane=%s:%s" % (names[0], test["script"]), port=port, **opts
            )
        return build_scenario_argv(
            cfg=cfg,
            host="lane=%s:%s" % (names[0], test["host"]),
            connect_ip="127.0.0.1",
            port=port,  # peers of ONE match share the host's port
            clients=[
                "lane=%s:%s" % (names[client_idx(i)], c) for i, c in enumerate(test["clients"])
            ],
            client_dead_ip=test.get("client_dead_ip"),
            # the local shim forwards to the host lane's port and listens on its own
            shim=shim_spec(test, "127.0.0.1:%d" % port, shim_port),
            **opts,
        )
    # mp:D29 (D3): a VM keeps every satellite unless told to omit it
    opts["omit_satellite"] = list(test.get("omit_satellite") or [])
    if test["kind"] == "solo":
        # host-only on the first VM: the same environment as the multi rows
        return build_scenario_argv(host="%s:%s" % (vms[0], test["script"]), cfg=cfg, **opts)
    host_ip = vms[0]
    return build_scenario_argv(
        cfg=cfg,
        host="%s:%s" % (host_ip, test["host"]),
        connect_ip=host_ip,
        clients=["%s:%s" % (vms[client_idx(i)], c) for i, c in enumerate(test["clients"])],
        client_dead_ip=test.get("client_dead_ip"),  # S8(b)
        shim=shim_spec(test, host_ip),
        **opts,
    )


def required_vms(test, vms):
    if test["kind"] == "solo":
        return [vms[0]]  # host-only on the first VM
    # mp:GS1(b): a reused client (client_shares_lane) does not cost an extra VM -- it relaunches on
    # the machine an earlier client already used, same as it reuses that peer's LOCAL lane.
    return vms[: 1 + len(client_lane_slots(test))]


# ---- INSTALL-TIME REFUSALS: the shared cause behind a whole-suite failure ------------------------
#
# 2026-09-01 (dead-ends G99): the effects gate took llm_gfx_present_flip's entry, the present hook was
# refused it, and since capture / the UI automation driver / the overlay ALL piggyback on_present the
# whole harness went dark. What the suite printed was seventeen scenarios each burning its full budget
# on "did not present a frame" -- identical timeouts describing a UI regression that did not exist.
# The cause was one line in the run's OWN mh_net.log, written before the first script step, and
# nothing was reading it.
#
# The gate that caused G99 is gone (fork F2F dropped the deferred-effect machinery, so the present
# hook owns that entry outright and its `[effects] PARTIALLY ARMED` needle went with it). The
# present-hook needle STAYS: a promotion or a new detour can take the entry the same way, and the
# consequence for the suite is identical.
INSTALL_REFUSALS = (
    (
        "present hook NOT armed",
        "the present hook lost its entry -- capture, the UI automation driver and the overlay ALL "
        "piggyback on_present, so NO scenario in this suite can capture a frame",
    ),
)

# WHAT IS DELIBERATELY *NOT* A NEEDLE, because the first draft of this table got it wrong and the
# mistake is the interesting half. `[interlock] install_trampoline at ... ` looks like the ideal
# match -- it is the literal line that named the outage -- but a HEALTHY run carries one too:
# `install_trampoline at 0049D8EF DISPLACED`, the MP D14 resync detour whose target is promoted,
# which is expected and benign. Matching it would abort the whole suite on any ordinary
# single-scenario UI regression and blame an unrelated line for it -- a check whose candidate set
# is not its verdict, which is the shape this repo keeps having to unlearn. So the table matches
# only conditions that are install-WIDE and harness-FATAL: a lost present hook -- nothing can
# capture. (It had a sibling, `[effects] PARTIALLY ARMED`, until fork F2F deleted that layer.)
# Confirmed absent from a real healthy log before being trusted.


def install_refusal_lines(body):
    """[(why, line)] for every install-time refusal in one mh_net.log body. Empty when it is clean.

    Deliberately a pure text function: the rig half (finding the newest log per lane) is untestable
    without a rig, and this half is the half that decides. Mutation-tested by --selftest-refusals."""
    out = []
    for needle, why in INSTALL_REFUSALS:
        if needle in body:
            line = next((ln.strip() for ln in body.splitlines() if needle in ln), needle)
            out.append((why, line[:200]))
    return out


def selftest_refusals():
    """Prove the detector fires -- and, just as much, that it stays QUIET on a healthy log.

    The quiet half is the one that matters most here. This detector ABORTS the suite, so a false
    positive costs every remaining scenario and points the reader at the wrong line. The healthy
    fixture below is copied from a real passing run and deliberately includes the benign
    `install_trampoline at ... DISPLACED` line that the first draft of the table matched."""
    ok = True
    healthy = (
        "; [interlock] install_trampoline at 0049D8EF DISPLACED -- that entry is inside "
        "llm_net_lockstep_broadcast_resync_state, which is PROMOTED in this run\n"
        "; present hook armed (own detour): frametime_log=1 eager_advertise=1\n"
        "; [interlock] 1 detour install(s) REFUSED -- [the resync detour @0049D8EF]\n"
    )
    hits = install_refusal_lines(healthy)
    if hits:
        print("SELFTEST FAIL: a healthy log reported a refusal -- %r" % hits)
        ok = False
    for body, want in (("; present hook NOT armed -- see the [interlock] line\n", "present hook"),):
        hits = install_refusal_lines(body)
        if not hits:
            print("SELFTEST FAIL: missed the refusal in %r" % body[:60])
            ok = False
        elif want not in hits[0][0]:
            print("SELFTEST FAIL: wrong reason for %r -- %s" % (body[:40], hits[0][0]))
            ok = False
    print("[%s] test_ui install-refusal detector" % ("ok" if ok else "FAIL"))
    return 0 if ok else 1


# Old test_ui.py mode flags -> the script that owns the mode now, in the old dispatch order (the
# first flag present wins, as the old if-chain did). None = this runner. (tooling:TL-SUITE-SPLIT)
MODE_FLAGS = (
    ("--det-selftest", "det_arms"),
    ("--check-budgets", None),
    ("--classify-selftest", None),
    ("--loadred-selftest", None),
    ("--sp-determinism", "det_arms"),
    ("--tact-equiv", "tact_test"),
    ("--tact-verify", "tact_test"),
    ("--tact-replay", "tact_test"),
    ("--tact-arm", "tact_test"),
    ("--selftest-refusals", None),
    ("--tact-determinism", "tact_test"),
    ("--tact-play", "tact_test"),
    ("--ui-selftest", "ui_abc"),
    ("--ui-oracle", "ui_abc"),
    ("--ui-abc", "ui_abc"),
    ("--ui-equiv", "ui_abc"),
    ("--ui-replay", "ui_abc"),
    ("--ui-play", "ui_abc"),
    ("--soak", "soak_test"),
    ("--tact-trim", "tact_test"),
    ("--tact-suite", "tact_test"),
    ("--list", None),
    ("--l1f-ping3", "det_arms"),
    ("--u19j-gpfg3", "det_arms"),
    ("--u19j-gpfg3-unguarded", "det_arms"),
    ("--x2a-map-variant", "det_arms"),
    ("--det-standard", "det_arms"),
    ("--det-3peer", "det_arms"),
    ("--u19b-quit3", "det_arms"),
    ("--det-config1", "det_arms"),
    ("--det-config1-gate", "det_arms"),
    ("--determinism", "det_arms"),
)


def old_mode_script(argv):
    """The script an old `test_ui.py <mode flag>` command line belongs to, or None."""
    for flag, script in MODE_FLAGS:
        if any(a == flag or a.startswith(flag + "=") for a in argv):
            return script
    return None


def __getattr__(name):
    """One cycle of back-compat for importers of the pre-split module (tooling:TL-SUITE-SPLIT)."""
    for mod in ("ui_suite_common", "tact_test", "ui_abc", "soak_test", "det_arms"):
        m = importlib.import_module(mod)
        if name in vars(m):
            return vars(m)[name]
    raise AttributeError("module 'test_ui' has no attribute %r" % name)


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("only", nargs="*", help="run only these test names (default: all)")
    ap.add_argument(
        "--check-budgets",
        action="store_true",
        help="lint: every TESTS row has budget_s, and one over %d s has a long_why (gate diet); a "
        "raised budget_s also needs budget_evidence (tooling:TL-SUITE-TIMEOUT-CLASS)"
        % BUDGET_LONG_S,
    )
    ap.add_argument(
        "--classify-selftest",
        action="store_true",
        help="tooling:TL-SUITE-TIMEOUT-CLASS: the timeout classifier's own negative cases, over "
        "synthetic run dirs -- no rig",
    )
    ap.add_argument(
        "--loadred-selftest",
        action="store_true",
        help="tooling:TL-SUITE-LOADRED: apply_load_rerun's own negative cases against a fake "
        "run_one -- no rig",
    )
    ap.add_argument(
        "--walk-watchdog-selftest",
        action="store_true",
        help="tooling:TL-HARN19: run_ui_test_watched's own negative cases against a fake child "
        "process -- no rig",
    )
    ap.add_argument(
        "--no-load-rerun",
        dest="load_rerun",
        action="store_false",
        default=True,
        help="tooling:TL-SUITE-LOADRED: skip the end-of-run solo rerun of red rows (debugging a red "
        "suite only -- a LOAD-RED row would otherwise re-run and print again). Default: on, capped "
        "at %d reruns." % LOAD_RERUN_CAP,
    )
    ap.add_argument(
        "--only", dest="only_flag", action="append", default=[], help="alias for a positional name"
    )
    ap.add_argument("--list", action="store_true", help="list the tests and exit")
    ap.add_argument(
        "--plant-desync",
        action="store_true",
        help="tooling:TL-SUITE-HASHDEF: flip the host's strategic PRNG once (ui_registry.PLANT_HOST) "
        "on every hashing row; a row PASSES only if its hash compare catches it. Opted-out rows SKIP.",
    )
    ap.add_argument(
        "--solo",
        action="store_true",
        help="run only the single-peer tests (host-only on the first VM)",
    )
    ap.add_argument(
        "--selftest-refusals",
        action="store_true",
        help="prove the install-time refusal detector fires (and stays quiet on a healthy log) "
        "without touching the rig; the lint gate runs this",
    )
    # LOCAL IS THE DEFAULT (2026-07-29). One lane folder per peer per test on THIS box, implying
    # --headless. Two reasons it is the default rather than an option: it isolates every test from the
    # setup.dat / [video] state the previous one left behind, and it FREES THE VMs -- so the UI suite
    # and a 2-machine determinism run stop contending for the same two peers and can run at once.
    # --no-local restores the old behaviour. NOT named --vms: that flag already exists and carries the
    # VM IP LIST, so reusing the name would be an argparse conflict, not an override.
    ap.add_argument(
        "--local",
        action="store_true",
        default=True,
        help="(default) run the whole suite on THIS machine instead of the VMs, with one lane folder "
        "per peer per test (LANE_ROOT/ui_<test> for a solo test, ui_<test>_host + ui_<test>_cN for a "
        "multi-peer one). Implies --headless.",
    )
    ap.add_argument(
        "--no-local",
        dest="local",
        action="store_false",
        help="opt OUT of --local: run the suite across the real peer VMs listed by --vms. Use when the "
        "thing under test is machine-dependent (a cross-machine transport or timing question); a pure "
        "UI/render regression does not need it and pays VM latency plus contention with any rig run.",
    )
    # PARALLEL BY DEFAULT (2026-08-02). Measured on this box (8C/16T): 12/12 in 1.8 min at --jobs 4
    # against 4.5 min serial, 2.5x, same verdicts. Each test owns its lane folders, its port and its
    # mutex number, so the only shared resource is the CPU.
    #
    # "CORRECTNESS RUNS ONLY" still holds and is now enforced by CONTROL FLOW rather than by the
    # caller remembering: --determinism and --sp-determinism both return from main() before `jobs` is
    # read at all, so contention cannot reach a run that measures pacing. Do not move those dispatches
    # below the jobs loop.
    #
    # Scaled by CPU rather than pinned to 4: this file is also run on the peer VMs and on whatever
    # box comes next, and a fixed 4 on a 4-core machine means ~8 game instances fighting over 4
    # cores, which is how a correctness suite starts producing timeouts that look like failures.
    ap.add_argument(
        "--net-jobs",
        type=int,
        default=4,
        metavar="N",
        help="the multi-peer tests' OWN pool width (default %(default)d). Separate from --jobs "
        "because a capped multi-peer test is wait-bound ([video] fps_cap: its peers sleep on the "
        "lockstep clock or on each other), so it should not hold a CPU-bound --jobs slot. Bounded "
        "by the number of multi tests; --jobs 1 disables the split entirely.",
    )
    ap.add_argument(
        "--jobs",
        type=int,
        # One core in four runs a solo lane; the only clamp is the floor. (Was also capped at 4,
        # which with the scaling term made the default a double clamp nobody could read at a
        # glance -- user, 2026-09-10. Big boxes were the cap's only subjects, and per-lane
        # isolation is exactly per-lane, so they can simply have the width.)
        default=max(1, (os.cpu_count() or 4) // 4),
        help="run this many TESTS concurrently (requires --local, which is the default; default "
        "scales with CPU count, %(default)d here). Note the peer count exceeds --jobs: a multi-peer "
        "test launches 2-3 game processes, so --jobs 4 can mean ~8 instances. Output is buffered per "
        "test and printed whole on completion. Pass --jobs 1 to serialise, e.g. when a failure's "
        "interleaved logs are hard to read.",
    )
    ap.add_argument(
        "--walk-stall-timeout",
        type=int,
        default=WALK_STALL_S,
        metavar="S",
        help="tooling:TL-HARN19 -- --local runs only: kill a scenario early when no lane's "
        "mh_uidrive.log has advanced for this many seconds (default %(default)d), instead of only "
        "at its flat `timeout`. A lane that is still writing, however slowly, is never killed by "
        "this; 0 disables it (flat-timeout-only, the pre-TL-HARN19 behaviour).",
    )
    # The AUDIT LEVER (2026-09-01): --net-extra reaches only [net] keys and per-scenario extra_ini
    # lives in the TESTS registry, so a suite-wide [tombstone] arm_dead audit had NO route -- its
    # first "green" was vacuous (the fragment never reached any lane; the X-TOMB resolve session).
    ap.add_argument(
        "--extra-ini-all",
        help="append this ini fragment to EVERY suite scenario (the audit lever, e.g. "
        "tools/uiscripts/ini/tombstone_audit.ini); baselines are recorded without it",
    )
    ap.add_argument("--tol", type=float, default=0.02)
    ap.add_argument("--pixdelta", type=int, default=40)
    add_runner_args(ap)
    add_net_args(ap)
    add_extra_ini_arg(ap)  # read by the pooled tactical-journal tail
    return ap


def select_tests(ap, args):
    """(tests to run, the names asked for) -- the registry filtered by --only / --solo / optin."""
    wanted = set(args.only) | set(args.only_flag)
    # `optin` (mp:X1b): a registered scenario that the DEFAULT suite does not run, and it is a
    # narrow door rather than a general one. It exists for a scenario whose MECHANISM is proven and
    # whose run still ends red for a reason that belongs to another tracker item -- mp_snapshot moves
    # a world between two live peers and verifies it region for region in its post_check, and then
    # the importing peer faults stepping the world it just imported, which is mp:X3's to fix. The
    # alternatives are both worse: leaving it in the default suite reds the gate for everyone on a
    # known, tracked cause, and not registering it at all loses the scripts, the lanes and the
    # baselines. Named with --only it runs exactly as before. Drop the key when its cause is fixed.
    if wanted:
        tests = [t for t in TESTS if t["name"] in wanted]
    else:
        tests = [t for t in TESTS if not t.get("optin")]
    if args.solo:
        tests = [t for t in tests if t["kind"] == "solo"]
    if wanted:
        missing = wanted - {t["name"] for t in TESTS}
        if missing:
            ap.error("unknown test name(s): %s (see --list)" % ", ".join(sorted(missing)))

    return tests, wanted


def print_list():
    print("UI tests (%d):" % len(TESTS))
    for t in TESTS:
        peers = "solo" if t["kind"] == "solo" else "host+%d client(s)" % len(t["clients"])
        print("  %-14s [%s]  %s" % (t["name"], peers, t["desc"]))
    print("  (special) determinism  UI-path lockstep hash check -- `--determinism [--steps N]`")
    print(
        "Tactical journal scenarios (%d) -- `--tact-suite`, NOT in the default suite:"
        % len(TACT_SCENARIOS)
    )
    for t in TACT_SCENARIOS:
        print("  %-14s [%s]  %s" % (t["name"], "+".join(t["arms"]), t["desc"]))
    return 0


def suite_options(args):
    """The suite-wide half of every registry row's ui_test argv (build_argv resolves overrides)."""
    headless = not args.visible
    suite = dict(
        timeout=args.timeout,
        tol=args.tol,
        pixdelta=args.pixdelta,
        update_baselines=args.update_baselines,
        # the audit lever: one fragment on EVERY scenario (baselines are recorded without it)
        extra_ini_all=args.extra_ini_all,
        # mp:T1: registry rows get --net-extra too (a row's own net_extra replaces it)
        net_extra=args.net_extra,
        headless=headless,
        timeout_frames=None,
    )
    return suite


def run_rows(args, cfg, tests, suite, plan, precondition_skip, jobs):
    """Run every row (two pools under --jobs > 1), then the solo rerun of the reds. Returns the
    verdicts, timings, hash verdicts, lane-share chains, abort cause and rerun record."""

    # ---- SHARED-CAUSE PREFLIGHT: an INSTALL-time entry loss, not 17 UI failures -----------------
    #
    # 2026-09-01 (dead-ends G99): the effects gate took llm_gfx_present_flip's entry, the present
    # hook was refused it, and since capture / UI drive / overlay ALL piggyback on_present the whole
    # harness went dark. What the suite printed was seventeen scenarios each burning its full budget
    # on "did not present a frame" -- identical timeouts describing a UI regression that did not
    # exist. The cause was one line in the run's OWN mh_net.log, written before the first script
    # step, and nothing was reading it.
    #
    # So: on the FIRST failure, read the install log. A reason that is install-WIDE will fail every
    # remaining scenario the same way, and running them proves nothing -- abort and NAME it. One
    # failure with a cause beats seventeen without one.
    def install_refusals(test):
        """Install-time refusals in this test's lanes, newest run each. [] when the logs say nothing.

        Read from the LANE's own mh_net.log rather than from ui_test's stdout: the refusal is
        printed by the injected DLL at install time, before any harness output exists to carry it."""
        found = []
        if plan is None:
            return found  # --no-local: the logs are on the VMs, not reachable from here
        _, _, lane_names = plan.get(test["name"], (None, None, []))
        for lane in lane_names:
            logs = sorted(
                glob.glob(os.path.join(make_lane.LANE_ROOT, lane, "logs", "*", "mh_net.log")),
                key=os.path.getmtime,
            )
            if not logs:
                continue
            try:
                with open(logs[-1], encoding="utf-8", errors="replace") as fh:
                    body = fh.read()
            except OSError:
                continue
            for why, line in install_refusal_lines(body):
                found.append((lane, why, line))
        return found

    # ---- PER-TEST POST-CHECK: an assertion the pixels cannot carry (fork F4A) -------------------
    #
    # A `[uitest]` script's predicates read UI STATE, so a scenario can assert what is on screen and
    # nothing else. Some claims are about the RUN rather than the frame -- F4A's is "a missing
    # sibling DLL refused loudly and the arm still finished", which has no pixels at all and whose
    # capture stays green whether or not the bind was even attempted. A test may therefore name a
    # command; it is run against that test's LANE after a green run, and a non-zero exit turns the
    # test red with the command's own output attached.
    #
    # The lane path is APPENDED rather than written into the entry: the runner is what knows which
    # lane a test got, and a hardcoded path in the registry would be a second answer that drifts.
    # Checkers take a lane folder and resolve its newest run themselves.
    def post_check(test):
        """(ok, text) for a test's `post_check`, or (True, "") when it has none / cannot run."""
        cmd = test.get("post_check")
        if not cmd:
            return True, ""
        # ONE COMMAND OR SEVERAL (fork F4D). A boot now has two satellites to make statements about,
        # and `libmh_absent` has to assert BOTH -- mh_net bound, libmh absent -- or it would pass on
        # a lane that lost the transport as well. Nested lists mean "run all of these"; the flat
        # form every existing entry uses is unchanged.
        cmds = cmd if isinstance(cmd[0], (list, tuple)) else [cmd]
        if len(cmds) > 1:
            ok_all, texts = True, []
            for c in cmds:
                ok, text = post_check(dict(test, post_check=list(c)))
                ok_all = ok_all and ok
                texts.append(text)
            return ok_all, "\n".join(t for t in texts if t)
        cmd = list(cmds[0])
        if plan is None:
            # --no-local: the lane logs live on the VM and this box cannot read them. SKIPPED and
            # SAID SO -- a silent pass here would make the local and VM topologies disagree about
            # what the suite proved.
            return (
                True,
                "  post-check SKIPPED: %s needs the local lane logs (--no-local run)" % cmd[0],
            )
        _, _, lane_names_ = plan.get(test["name"], (None, None, []))
        if not lane_names_:
            return False, "  post-check FAILED: no lane recorded for %s" % test["name"]
        lane_dir = os.path.join(make_lane.LANE_ROOT, lane_names_[0])
        # SES1: hand over the PROCESS ("menu") run directory, not the lane. Every post-check here is
        # about a BOOT-TIME fact -- which satellites bound at DllMain -- and those lines are written
        # before any lobby exists. check_module_bind resolves a bare lane to its newest run by mtime,
        # which since SES1 is the newest SESSION directory the scenario opened: menu_walk creates a
        # game, so its newest folder holds the match's logs and no `[modules]` line at all, and the
        # check REFUSED (correctly -- it will not pass a log it cannot see the bind in).
        # mp:D28: a checker whose evidence is match-time-only mh_net.log content (not a boot-time
        # banner, not mh_harness.log -- both live in the process dir regardless) needs the SESSION
        # directory, the inverse of every checker above it. Opt in per-row with
        # `post_check_session: True` rather than changing sp_newest_run's default for the ~50 rows
        # that rely on it (see sp_newest_session_run's own comment for why check_cancel_task.py's
        # `routed as order` line specifically cannot be found in the process dir).
        newest = sp_newest_session_run if test.get("post_check_session") else sp_newest_run
        target = newest(lane_dir) or lane_dir
        # mp:X1b. A CROSS-PEER post-check gets EVERY peer's run directory, not only the host's.
        # Every existing checker asks a question about one process (did this boot bind that module),
        # so one lane was the right argument and still is. "Did the world one peer captured arrive
        # in the other peer's memory" cannot be asked of a single log by construction: the capture
        # is in one file and the import in another, and a checker handed one of them can only ever
        # report half the claim -- which would read as a pass.
        if test.get("post_check_peers"):
            targets = []
            for nm in lane_names_:
                d = os.path.join(make_lane.LANE_ROOT, nm)
                targets.append(newest(d) or d)
        else:
            targets = [target]
        # tooling:TL-SUITE-TIMEOUT-CLASS -- the one artifact ui_test.py/the DLL cannot write, because
        # whether the ASSERTION started is a fact about THIS function's own control flow (it never
        # even runs on rc != 0), not about anything the game process did. Best-effort: a marker that
        # fails to write must never be why a post-check itself fails.
        for _d in targets:
            try:
                open(os.path.join(_d, POST_CHECK_MARKER), "w").close()
            except OSError:
                pass
        argv = [sys.executable, os.path.join(REPO, *cmd[0].split("/"))] + list(cmd[1:]) + targets
        r = subprocess.run(argv, capture_output=True, text=True)
        out = (r.stdout or "") + (r.stderr or "")
        head = "  post-check %s: %s" % ("PASS" if r.returncode == 0 else "FAILED", " ".join(cmd))
        return r.returncode == 0, head + "\n" + "\n".join("    " + ln for ln in out.splitlines())

    def test_run_dirs(test):
        """The newest process directory of each of this test's lanes (TIMEOUT-CLASS evidence). [] when the lane logs are not reachable from here
        (--no-local) or no lane was ever provisioned, the same two reasons post_check gives up; a
        TIMEOUT-CLASS read of an empty list is NEVER-STARTED, which is the right answer either way.
        Deliberately separate from post_check (read-only, no side effect) so classification can run
        on every scenario, not only the ones that reached a post_check call."""
        if plan is None:
            return []
        _, _, lane_names_ = plan.get(test["name"], (None, None, []))
        if not lane_names_:
            return []
        # The PROCESS dir of every lane: captures + mh_uidrive.log live there, never in a match's
        # session dir (reading the session dir called post_check_session rows NEVER-STARTED).
        return [
            sp_newest_run(os.path.join(make_lane.LANE_ROOT, nm))
            or os.path.join(make_lane.LANE_ROOT, nm)
            for nm in lane_names_
        ]

    hashes = {}  # name -> short hash verdict for the summary (tooling:TL-SUITE-HASHDEF)

    def hash_check(test):
        """(ok, text, short) -- tooling:TL-SUITE-HASHDEF: mp_analyze over every peer's process dir."""
        if test.get("kind") != "multi":
            return True, "", ""
        if ui_registry.hash_plan(test) is None:
            return True, "  hash: not compared -- %s" % test["no_hash_why"], "opt-out"
        if plan is None:
            return True, "  hash: SKIPPED -- needs the local lane logs (--no-local run)", "skipped"
        _, _, lanes = plan.get(test["name"], (None, None, []))
        dirs = [sp_newest_run(os.path.join(make_lane.LANE_ROOT, nm)) for nm in lanes]
        if len(dirs) < 2 or None in dirs:
            return False, "  hash: FAIL -- no run dir for 2+ peers (lanes %s)" % lanes, "FAIL"
        for d in dirs:
            try:
                open(os.path.join(d, POST_CHECK_MARKER), "w").close()
            except OSError:
                pass
        plant = ui_registry.PLANT_STEP if args.plant_desync else 0
        ok, line, out = ui_registry.hash_compare(
            dirs, os.path.join(dirs[0], "mp_analyze.json"), plant
        )
        text = "  " + line
        if not ok:
            text += "\n" + "\n".join("    " + ln for ln in out.splitlines()[-25:])
        short = line.split(" -- ")[0].replace("hash: ", "")
        if ok and not plant:
            short = "IDENTICAL/%s" % line.split("over ")[1].split()[0]
        return ok, text, short

    def run_one(idx, t):
        """Run one test. Returns (name, verdict, text-to-print). Safe to call from a worker thread."""
        head = (
            "\n"
            + "=" * 78
            + "\n[%d/%d] %s (%s) -- %s\n"
            % (
                idx,
                len(tests),
                t["name"],
                t["kind"],
                t["desc"],
            )
            + "=" * 78
        )
        if jobs == 1:
            print(head)
        if t["name"] in precondition_skip:
            msg = "  SKIP -- %s" % precondition_skip[t["name"]]
            if jobs == 1:
                print(msg)
            return t["name"], "SKIP", head + "\n" + msg
        if abort:
            msg = (
                "  SKIP -- suite aborted: %s hit an INSTALL-TIME refusal, which fails every "
                "scenario identically. Fix that first; this test was never run." % abort[0]
            )
            if jobs == 1:
                print(msg)
            return t["name"], "SKIP", head + "\n" + msg
        need = [] if args.local else required_vms(t, args.vms)
        down = [ip for ip in need if not vm_reachable(ip)]
        if down:
            msg = "  SKIP -- VM(s) unreachable: %s" % ", ".join(down)
            if jobs == 1:
                print(msg)
            return t["name"], "SKIP", head + "\n" + msg
        # tooling:TL-SUITE-HASHDEF: a multi row arms the hash harness unless it declares no_hash_why.
        hextra = ui_registry.hash_plan(t)
        if hextra is not None:
            t = dict(t, harness_extra=hextra)
            if args.plant_desync:
                t["harness_extra_host"] = ui_registry.harness_merge(
                    t.get("harness_extra_host"), ui_registry.PLANT_HOST
                )
        elif args.plant_desync:
            msg = "  SKIP -- --plant-desync: this row does not hash (%s)" % (
                t.get("no_hash_why") or "solo"
            )
            if jobs == 1:
                print(msg)
            return t["name"], "SKIP", head + "\n" + msg
        # The wrapper kill-timeout must outlast the test's OWN wall-clock budget, or it kills the run
        # before ui_test can report -- and a killed run has no verdict, only a missing one.
        t0 = time.time()
        # mp:R2 -- a relayed scenario runs against a relay process started for it, here, and told
        # to the peers as a `[net] relay=` knob. Unconditional context manager so the cleanup path
        # is the same whether or not this test wants one.
        relay_note = ""
        # mp:R4b -- `relay_restart_on: <signal>`: the relay is killed and respawned on the same port
        # when a peer's script emits that signal (ui_test's --signal-touch drops the request file).
        restart_req = (
            os.path.join(REPO, "tmp", "relay_%s.restart" % t["name"])
            if t.get("relay_restart_on")
            else None
        )
        with (
            RelayProc(
                RELAY_PORT,
                os.path.join(REPO, "tmp", "relay_%s.log" % t["name"]),
                restart_request=restart_req,
                extra_args=t.get("relay_args"),
            )
            if t.get("relay")
            else contextlib.nullcontext()
        ) as relay:
            if t.get("relay"):
                if not relay.ok:
                    msg = "  SKIP -- the relay could not be started: %s" % relay.note
                    if jobs == 1:
                        print(msg)
                    return t["name"], "SKIP", head + "\n" + msg
                knob = "relay=%s:%d" % (relay_addr_for_peers(args.local), relay.port)
                # mp:R7a -- `relay_client_only: True` puts `relay=` on the CLIENT peers ONLY, so the
                # HOST never registers and the relay stays truly idle (peers=0). That is what
                # direct_dial_with_relay_set needs: a relay that EXISTS to be NOT contacted, proving the
                # client's *Internet server* + typed-IP dial went direct. Every other relay scenario
                # wants both peers on the relay (net_extra), which is the default.
                if t.get("relay_client_only"):
                    t = dict(
                        t,
                        net_extra_client=(t.get("net_extra_client", "") + ";" + knob).lstrip(";"),
                    )
                else:
                    t = dict(
                        t, net_extra=(t["net_extra"] + ";" + knob) if t.get("net_extra") else knob
                    )
                if restart_req:
                    t["signal_touch"] = "%s=%s" % (t["relay_restart_on"], restart_req)
                relay_note = "  [relay %s -> tmp/relay_%s.log]" % (relay.note, t["name"])
                if jobs == 1:
                    print(relay_note)
            # tooling:TL-HARN19 -- local runs get the progress watchdog (killed only once no lane's
            # mh_uidrive.log has advanced for --walk-stall-timeout seconds); VM/--no-local runs keep
            # the flat kill unchanged (test_run_dirs needs the local lane logs to read progress from).
            if args.local and plan is not None:
                rc, text = run_ui_test_watched(
                    build_argv(t, args.vms, suite, plan, cfg, local=True),
                    max(args.per_test_timeout, _walk_backstop_timeout(t, suite) + 120),
                    lambda _t=t: test_run_dirs(_t),
                    stall_s=args.walk_stall_timeout,
                    capture=jobs > 1,
                )
            else:
                rc, text = run_ui_test(
                    build_argv(t, args.vms, suite, plan, cfg),
                    max(args.per_test_timeout, t["timeout"] + 120)
                    if t.get("timeout")
                    else args.per_test_timeout,
                    capture=jobs > 1,
                )
        if relay_note:
            text = (text or "") + "\n" + relay_note
        # Wall clock per test, because "the suite takes too long" is not actionable until you can see
        # WHICH test spends the time. Pairs with the per-step milliseconds ui_drive now logs.
        #
        # D15: report the wall clock AGAINST ITS BUDGET, not alone. A bare "[203s]" is unreadable --
        # you cannot tell a slow pass from a run that spent its entire budget waiting and then aborted,
        # and those are opposite findings. The budget is the ui_test --timeout this test was actually
        # given (its own override, else the suite default), which is the number that kills it.
        # A test over WARN_FRAC of its budget is a NEAR-MISS: still green, already unsafe, and the
        # thing that turns red first when the machine is busier. Printing it is what makes the drift
        # visible BEFORE it is a failure, which is exactly what nobody had on 2026-08-06.
        secs = time.time() - t0
        budget = t.get("timeout") or args.timeout
        frac = secs / budget if budget else 0.0
        el = "  [%.0fs / %ds budget, %.0f%%]" % (secs, budget, frac * 100)
        if frac >= WARN_FRAC:
            el += "  <-- NEAR-MISS: over %.0f%% of budget" % (WARN_FRAC * 100)
        if jobs == 1:
            print(el)
        timings[t["name"]] = (secs, budget)
        tail = el
        if rc != 0:
            refusals = install_refusals(t)
            if refusals:
                banner = "\n" + "!" * 78 + "\n"
                banner += (
                    "INSTALL-TIME REFUSAL in this run's own mh_net.log -- this is NOT a UI "
                    "failure, and every remaining scenario would fail the same way.\n"
                )
                for lane, why, line in refusals:
                    banner += "  [%s] %s\n      %s\n" % (lane, why, line)
                banner += (
                    "Two mechanisms wanted one entry; the log names the WRONG remedy, and "
                    "the suite then reads as N unrelated timeouts.\n"
                )
                banner += "!" * 78
                # PRINT IT HERE, not only into the returned text: under --jobs 1 the caller
                # discards that text and prints as it goes, so a banner that only rode home in
                # the return value would be invisible in exactly the serial run someone
                # debugging a dead suite reaches for first.
                if jobs == 1:
                    print(banner)
                tail += banner
                abort.append(t["name"])
        if rc == 0:
            # Only on a green run: a failed scenario's lane has nothing worth asserting about, and a
            # post-check red on top of a scenario red would name the wrong cause.
            ok, ptext = post_check(t)
            if ptext:
                if jobs == 1:
                    print(ptext)
                tail += "\n" + ptext
            if not ok:
                rc = 1
        # tooling:TL-SUITE-HASHDEF: the lockstep hash compare. Under --plant-desync it IS the verdict.
        if rc == 0 or args.plant_desync:
            hok, htext, hashes[t["name"]] = hash_check(t)
            if htext:
                if jobs == 1:
                    print(htext)
                tail += "\n" + htext
            if args.plant_desync:
                rc = 0 if hok else 1
            elif not hok:
                rc = 1
        # tooling:TL-SUITE-TIMEOUT-CLASS -- classify from the run's OWN ARTIFACTS before deciding the
        # verdict word, so a timeout never reads as a bare FAIL (G282/TL-HARN19: a budget red and a
        # determinism red used to be the same word). timed_out is read off this run's own text/rc;
        # cap/harn/walk/pcs are read off every lane this test touched, OR'd across peers -- one peer
        # that never booted is enough to call the whole scenario NEVER-STARTED.
        timed_out = looks_like_timeout(rc, text, secs=secs, kill_bound=budget)
        cap = harn = walk = pcs = False
        for _rd in test_run_dirs(t):
            c, h, w, p = run_artifacts(_rd)
            cap, harn, walk, pcs = cap or c, harn or h, walk or w, pcs or p
        verdict = classify_result(rc, timed_out, secs, t.get("budget_s"), cap, harn, walk, pcs)
        if verdict in TIMEOUT_CLASSES:
            tail += "\n  TIMEOUT-CLASS: %s -- %s" % (verdict, TIMEOUT_CLASSES[verdict])
            if verdict == "WALK-STALLED":
                for _rd in test_run_dirs(t):
                    tail += "\n    last step [%s]: %s" % (
                        os.path.basename(os.path.dirname(os.path.dirname(_rd))) or _rd,
                        last_script_step(_rd) or "(no script line)",
                    )
        elif verdict == "SLOW" and rc == 0:
            tail += (
                "\n  TIMEOUT-CLASS: SLOW -- passed, but %.0fs is over %.1fx its budget_s %ds"
                % (
                    secs,
                    SLOW_OVER_BUDGET,
                    t.get("budget_s") or 0,
                )
            )
        # A row carrying `expect_red: "<tracker id>"` is a REPRODUCTION of an open bug, registered
        # before its fix so the fix has a gate to turn green. Its red is the expected state (XFAIL,
        # not a failure); its green is the fix landing (XPASS, reported as a FAILURE so the key gets
        # removed the same session -- a scenario that passes while claiming to be red is a lie).
        if t.get("expect_red"):
            verdict = "XFAIL" if rc != 0 else "XPASS"
            tail += "\n  expect_red=%s -> %s%s" % (
                t["expect_red"],
                verdict,
                ""
                if rc != 0
                else " (the bug this row reproduces no longer reproduces: drop expect_red)",
            )
        return t["name"], verdict, head + "\n" + text + tail

    # Set by the first failure whose cause is install-wide; every later test then SKIPs instead of
    # re-proving it. A list rather than a flag so the abort can name WHICH test found it.
    abort = []
    results = {}
    chains = []  # the serial share_lanes groups, recorded for run_gate / gate_timeline
    timings = {}  # name -> (elapsed_s, budget_s); feeds the summary table's budget column
    if jobs == 1:
        for i, t in enumerate(tests, 1):
            name, verdict, _ = run_one(i, t)
            results[name] = verdict
    else:
        # Each test owns its own lane folder(s), port and mutex number, so the only shared resource is
        # the CPU. That makes this safe for CORRECTNESS runs and wrong for anything timing-sensitive --
        # the same rule headless already carries (the parallel-lane notes). Output is buffered per test
        # and printed whole on completion, in completion order.
        #
        # TWO POOLS SINCE 2026-09-10 (user: "I'd give them separate pool"). Since the [pacing]
        # fps_cap, a multi-peer test's peers SLEEP between frames -- their pace is the other peer
        # or the lockstep clock, not the CPU -- so one of them holding a --jobs slot for 30-95 s
        # starves the CPU-bound solo tests of a worker it barely uses. The multi tests get their
        # own wait-bound pool (--net-jobs wide) while --jobs stays the CPU-bound solo budget; the
        # machine-wide boot lock still serialises every launch instant, and per-lane isolation is
        # untouched. --jobs 1 keeps the fully-serial order for debugging (no split).
        solos = [(i, t) for i, t in enumerate(tests, 1) if t.get("kind") != "multi"]
        multis = [(i, t) for i, t in enumerate(tests, 1) if t.get("kind") == "multi"]

        def lane_groups(pool):
            """Connected components of "borrows lanes from", within ONE scheduling pool.

            mp:R7a -- a share_lanes test borrows a comparable scenario's lanes, so the two must
            NEVER run at once: same lane folder, same mh.dll, same setup.dat. Fold each sharer into
            its target's work item and run them SERIALLY in one worker; a sharer whose target is not
            in this run (a subset) stands alone on its own provisioned lanes. mp:R2b -- a sharer may
            borrow from SEVERAL targets (share_targets), hence components rather than pairs: targets
            first within a group (they own the lanes), sharers after, in registry order.

            THIS RUNS OVER BOTH POOLS since 2026-09-22 (tooling:TL-RIG-DEFANG's gate). It used to be
            written inline over `multis` only, because every sharer WAS multi -- and the day the
            first solo-to-solo sharer was registered (gx1_overlay_residue borrowing debug_overlay's
            lane) the solo pool happily ran the pair concurrently at --jobs 4 and the second one died
            copying mh.dll into a folder the first still had open: `PermissionError: [Errno 13]` at
            1s of a 200s budget. It passed every time it was run alone, which is how it was
            registered. A pool that cannot express "these two share a lane" must not be handed a
            sharer -- so the grouping is the pool's, not the multi branch's.
            """
            present = {t["name"] for _, t in pool}
            parent = {t["name"]: t["name"] for _, t in pool}

            def _find(n):
                while parent[n] != n:
                    parent[n] = parent[parent[n]]
                    n = parent[n]
                return n

            for i, t in pool:
                tg = share_targets(t)
                if tg and all(x in present for x in tg):
                    for x in tg:
                        parent[_find(x)] = _find(t["name"])
            # NO SHIM FOLDING since 2026-09-24. Shim rows used to be folded into ONE component
            # because every shim listened on the same control port (a machine-wide singleton:
            # txdeath_ingame died at bind against net_hud's shim, 2026-09-22 gate). Each local shim
            # row now carries its own control port (shim_control_port), so they run concurrently
            # like any other multi row; lane_alloc --check keeps the ports disjoint.
            out, by_root = [], {}
            for i, t in pool:
                by_root.setdefault(_find(t["name"]), []).append((i, t))
            for members in by_root.values():
                owners = [(i, t) for i, t in members if not share_targets(t)]
                sharers = [(i, t) for i, t in members if share_targets(t)]
                out.append(owners + sharers)
            return out

        groups = lane_groups(multis)
        solo_groups = lane_groups(solos)
        chains = [[t["name"] for _i, t in g] for g in groups + solo_groups if len(g) > 1]
        net_jobs = max(1, min(args.net_jobs, len(groups) or 1))
        print(
            "\nrunning %d tests: %d solo in %d lane group(s) at --jobs %d + %d multi-peer in "
            "their own pool (--net-jobs %d; capped peers wait more than they compute)"
            % (len(tests), len(solos), len(solo_groups), jobs, len(multis), net_jobs)
        )
        done = 0

        def run_group(grp):
            """Run one or more tests serially in a single worker (a share-lanes pair, or a solo group
            of one). Returns a list of (name, verdict, text)."""
            return [run_one(i, t) for i, t in grp]

        with (
            cf.ThreadPoolExecutor(max_workers=jobs) as ex_cpu,
            cf.ThreadPoolExecutor(max_workers=net_jobs) as ex_net,
        ):
            futs = {}
            futs.update({ex_cpu.submit(run_group, g): [t for _, t in g] for g in solo_groups})
            futs.update({ex_net.submit(run_group, g): [t for _, t in g] for g in groups})
            for f in cf.as_completed(futs):
                for name, verdict, text in f.result():
                    results[name] = verdict
                    done += 1
                    print(text)
                    print("  --> %s %s   (%d/%d complete)" % (name, verdict, done, len(tests)))

    # tooling:TL-SUITE-LOADRED -- every row has now finished, so the pool this ran under is idle:
    # rerunning a red row HERE is "alone" by construction, no --jobs override needed. Skipped after
    # an install-wide abort (every remaining row is a SKIP naming the same cause, not a real red).
    load_rerun = {}
    if args.load_rerun and not abort:

        def _run_one_alone(t):
            print("\n" + "-" * 78)
            print("solo rerun: %s" % t["name"])
            _, verdict, text = run_one(len(tests) + 1, t)
            if jobs > 1:  # jobs==1's run_one already streamed this as it ran
                print(text)
            secs, _budget = timings.get(t["name"], (0.0, 0))
            return verdict, secs

        load_rerun = apply_load_rerun(tests, results, timings, _run_one_alone)
        if load_rerun:
            n_capped = sum(
                1 for r_info in load_rerun.values() if r_info["outcome"] == "RED-NOT-RERUN"
            )
            print("\n" + "-" * 78)
            print(
                "solo rerun summary (tooling:TL-SUITE-LOADRED)%s:"
                % (
                    "  -- %d red row(s) over the %d-rerun cap, reported RED-NOT-RERUN (gate cost: "
                    "a red suite is already the slow path)" % (n_capped, LOAD_RERUN_CAP)
                    if n_capped
                    else ""
                )
            )
            for name, r_info in load_rerun.items():
                print(
                    "  %-14s first=%-16s rerun=%-16s -> %s%s"
                    % (
                        name,
                        r_info["first_verdict"],
                        r_info["rerun_verdict"] or "-",
                        r_info["outcome"],
                        ("  (%s)" % r_info["allow_reason"]) if r_info["allow_reason"] else "",
                    )
                )
    return types.SimpleNamespace(
        results=results,
        timings=timings,
        hashes=hashes,
        chains=chains,
        abort=abort,
        load_rerun=load_rerun,
    )


def print_summary(tests, rows):
    """The summary table. Returns (npass, nfail, nskip, nxfail)."""
    results, timings, hashes, load_rerun = rows.results, rows.timings, rows.hashes, rows.load_rerun
    print("\n" + "=" * 78)
    print("UI TEST SUITE")
    print("-" * 78)
    npass = nfail = nskip = 0
    nnear = nxfail = 0
    for t in tests:
        r = results[t["name"]]
        # tooling:TL-SUITE-TIMEOUT-CLASS -- SLOW is a real pass (it finished, its assertion ran) that
        # merely ran long; NEVER-STARTED/ASSERTION-NOT-RUN are timeouts that never got that far, so
        # they count as failures like any other red, just under their own more specific name.
        # tooling:TL-SUITE-LOADRED -- LOAD-RED-ALLOW is a pass-alone row the allow-list accepts;
        # LOAD-RED (not allow-listed) and RED-NOT-RERUN (over the rerun cap) still fail the gate,
        # same as any other red, just under their own name (never a bare FAIL for a load artifact).
        npass += r in ("PASS", "SLOW", "LOAD-RED-ALLOW")
        nfail += r in (
            "FAIL",
            "XPASS",
            *TIMEOUT_CLASSES,
            "LOAD-RED",
            "RED-NOT-RERUN",
        )  # an XPASS is a stale expect_red key: a failure until it is dropped
        nskip += r == "SKIP"
        nxfail += r == "XFAIL"
        # D15(b): the budget column lives in the SUMMARY, not only in the per-test block. Under --jobs
        # the per-test output is buffered and scrolls past; the summary is the part anyone actually
        # reads, so the near-miss has to be visible there or it is not visible at all.
        secs, budget = timings.get(t["name"], (0.0, 0))
        col = ""
        if budget:
            frac = secs / budget
            col = "  %5.0fs / %4ds  %3.0f%%" % (secs, budget, frac * 100)
            if frac >= WARN_FRAC and r != "SKIP":
                col += "  NEAR-MISS"
                nnear += 1
        # The routine's column (block 4b): expected cost, and the flag run_gate reds on. Skip the
        # OVER-BUDGET annotation when the verdict word already says SLOW -- one signal, not two.
        if t.get("budget_s") and r != "SKIP":
            col += "  exp %ds" % t["budget_s"]
            if secs > SLOW_OVER_BUDGET * t["budget_s"] and r != "SLOW":
                col += "  OVER-BUDGET (>%.1fx)" % SLOW_OVER_BUDGET
        if hashes.get(t["name"]):
            col += "  hash=%s" % hashes[t["name"]]
        if t["name"] in load_rerun:
            col += "  [solo rerun: %s]" % load_rerun[t["name"]]["outcome"]
        print("  %-14s %-5s%s" % (t["name"], r, col))
    print("-" * 78)
    print(
        "  %d passed, %d failed, %d skipped%s"
        % (
            npass,
            nfail,
            nskip,
            ("  (%d expected-red, see expect_red rows)" % nxfail) if nxfail else "",
        )
    )
    if nnear:
        # Loud on purpose: a suite that is 12/12 with three tests at 90% of budget is one busy machine
        # away from being 9/12, and the 12/12 line alone actively hides that.
        print(
            "  %d test(s) NEAR-MISS (>=%.0f%% of budget) -- green, but not by much"
            % (nnear, WARN_FRAC * 100)
        )
    return npass, nfail, nskip, nxfail


def write_suite_timing(args, tests, jobs, suite_t0, counts, rows):
    """The suite states its own cost (tmp/ui_test/last_suite_timing.json; run_gate reads it)."""
    npass, nfail, nskip, nxfail = counts
    results, timings, hashes, chains = rows.results, rows.timings, rows.hashes, rows.chains
    load_rerun = rows.load_rerun
    # WHY THIS EXISTS. "The UI suite costs ~30 minutes" was written into the project instructions,
    # this repo's DLL README, the ui-testing skill AND a memory record, and every one of them was ~6x wrong by
    # 2026-08-02 -- true when written, then quietly obsoleted by parallel lanes and headless runs. It
    # went unnoticed because nothing measured it: the figure was only ever prose citing prose. An
    # unattended session budgeting against it has a real reason to skip a gate it could easily afford.
    # So the suite states its own cost, and writes it where the next reader can find it without
    # running anything.
    elapsed = time.time() - suite_t0
    print("  %.1f min wall clock (%d test(s), %d at a time)" % (elapsed / 60, len(tests), jobs))
    try:
        rec = os.path.join(REPO, "tmp", "ui_test", "last_suite_timing.json")
        os.makedirs(os.path.dirname(rec), exist_ok=True)
        with open(rec, "w", encoding="utf-8") as fh:
            json.dump(
                {
                    "seconds": round(elapsed, 1),
                    "tests": len(tests),
                    "jobs": jobs,
                    "passed": npass,
                    "failed": nfail,
                    "skipped": nskip,
                    "expected_red": nxfail,
                    # gate diet block 4b: what run_gate's cost rules read
                    "net_jobs": getattr(args, "net_jobs", None),
                    # tooling:TL-SUITE-LOADRED -- both results visible: `verdict` below already
                    # carries the outcome word (LOAD-RED/LOAD-RED-ALLOW/RED-NOT-RERUN), this is the
                    # first run's verdict + both runs' timings for whoever is reading the record.
                    "load_rerun": load_rerun,
                    "per_test": {
                        t["name"]: {
                            "secs": round(timings.get(t["name"], (0.0, 0))[0], 1),
                            "budget_s": t.get("budget_s"),
                            "verdict": results.get(t["name"]),
                            "hash": hashes.get(t["name"]),
                        }
                        for t in tests
                    },
                    "chains": [
                        {
                            "tests": c,
                            "secs": round(sum(timings.get(n, (0.0, 0))[0] for n in c), 1),
                        }
                        for c in chains
                    ],
                },
                fh,
                indent=1,
            )
    except OSError:
        pass  # a timing note is never worth failing a suite over


def run_tact_tail(args, cfg, jobs):
    """The tactical journal scenarios pooled after the rows. Returns (passed, failed)."""
    npass = nfail = 0
    # THE TACTICAL JOURNAL SCENARIOS, IN THE DEFAULT SUITE (2026-09-04). They were registered but
    # excluded, on a cost argument -- "one is ~20 minutes against ~5 for all eleven capture
    # scenarios" -- that is now false: the arm costs ~22 s since the mission-end exit landed (G118),
    # so both arms of poz1_combat add well under a minute to a ~3 min suite.
    #
    # Excluding them was not free. The capture scenarios cannot see the tactical order path at all
    # -- `tact_panel` passes with the tact_frame promotion on OR off, while that promotion was
    # dropping 184 of 211 of the player's orders. The only arm that could see it sat unrun for two
    # days and a human found the bug by playing the game.
    #
    # `equiv`, not `verify`: the differential, for the reason run_tact_equiv documents at length --
    # the recording is not reproducible in this environment, so an absolute match count would be
    # permanently red or re-baselined into vacuity, while the differential asks the question that
    # actually matters and is mutation-proven to go red on the real defect.
    # POOLED SINCE 2026-09-10 (user: "recorded scenarios aren't parallel"). The four journals
    # used to run one-lane-serial AFTER the parallel test pool finished, which made this tail
    # the suite's longest fully-serial stretch. Each journal now gets its own lane slot (its
    # two arms stay serial WITHIN the journal -- the equiv's premise is both arms in the same
    # environment, and one process per journal keeps the machine inside the core budget);
    # provisioning self-serialises in make_lane and boots on the machine-wide boot lock.
    # Output is routed per THREAD into a buffer and printed whole on completion, the same
    # contract the test pool has -- interleaved journal logs are unreadable exactly when a
    # red needs reading. --jobs 1 keeps the serial tail for debugging.
    runnable = []
    for i, sc in enumerate(TACT_SCENARIOS):
        path = os.path.join(REPO, sc["journal"])
        if not os.path.isfile(path):
            print("  %-14s FAIL   (journal missing: %s)" % (sc["name"], sc["journal"]))
            nfail += 1
            continue
        sub = tact_test.tail_args(args, path)
        runnable.append((sc["name"], sub))

    # THE POOL IS THE `tact` LANE BLOCK, NOT THE SCENARIO LIST. The tail used to hand journal i
    # lane slot i and run four at once; the block was shrunk to two lanes on 2026-09-19 (the
    # capture suite needed the numbers, lane_alloc.py) and from then on every full-suite run
    # ended `IndexError: lane block 'tact' holds 2 lane(s) ... asked for index 3` on journals
    # 3 and 4 -- two reds that read as tactical regressions and were a lane bookkeeping bug.
    # Now a journal CLAIMS a slot from a pool as wide as the block when it starts and releases
    # it when it ends, so the tail runs as wide as the block allows and never wider.
    tact_width = lane_alloc.block("tact")[1]
    tail_jobs = 1 if jobs == 1 else max(1, min(4, tact_width, len(runnable)))
    tact_slots = queue.Queue()
    for _slot in range(tail_jobs):
        tact_slots.put(_slot)

    class _ThreadRouter:
        """stdout proxy routing write() by thread: registered threads write to their own
        buffer, everything else passes through. Keeps run_tact_equiv's prints intact while
        the pool runs several journals at once."""

        def __init__(self, real):
            self.real = real
            self.routes = {}

        def register(self, buf):
            self.routes[threading.get_ident()] = buf

        def unregister(self):
            self.routes.pop(threading.get_ident(), None)

        def write(self, s):
            self.routes.get(threading.get_ident(), self.real).write(s)

        def flush(self):
            self.routes.get(threading.get_ident(), self.real).flush()

        def __getattr__(self, a):
            return getattr(self.real, a)

    def _one_journal(name, sub):
        buf = _io.StringIO()
        router.register(buf)
        slot = tact_slots.get()  # a lane of the tact block, held for this journal's arms
        sub.tact_slot = slot
        try:
            rc = run_tact_equiv(sub, cfg)
        except Exception:
            import traceback

            traceback.print_exc()
            rc = 1
        finally:
            tact_slots.put(slot)
            router.unregister()
        return name, rc, buf.getvalue()

    if tail_jobs == 1:
        for name, sub in runnable:
            print("-" * 78)
            print("  tactical journal: %s" % name)
            sub.tact_slot = 0
            if run_tact_equiv(sub, cfg):
                nfail += 1
            else:
                npass += 1
    elif runnable:
        print("-" * 78)
        print(
            "running %d tactical journal(s), %d at a time (one lane each; output buffered "
            "per journal)" % (len(runnable), tail_jobs)
        )
        router = _ThreadRouter(sys.stdout)
        sys.stdout = router
        try:
            with cf.ThreadPoolExecutor(max_workers=tail_jobs) as ex:
                futs = [ex.submit(_one_journal, n, s) for n, s in runnable]
                for f in cf.as_completed(futs):
                    name, rc, text = f.result()
                    print("-" * 78)
                    print("  tactical journal: %s" % name)
                    print(text, end="")
                    print("  --> %s %s" % (name, "FAIL" if rc else "PASS"))
                    if rc:
                        nfail += 1
                    else:
                        npass += 1
        finally:
            sys.stdout = router.real
    return npass, nfail


def run_suite(args, cfg, tests, wanted):
    suite = suite_options(args)
    # mp:F2b (and any future opt-in scenario): a test may declare `requires(args) -> (ok, reason)`.
    # Checked ONCE here, before provisioning -- this cannot simply live inside run_one, because
    # provisioning happens up front for every test in `tests`, and letting make_lane.py fail on an
    # unmet precondition (e.g. font_merged's --src does not exist) would abort EVERY test's lanes
    # with it (provision_lanes returns None on any single lane failure). `tests` itself stays the
    # full list -- the summary and the run loop below still print a row for a skipped test; only
    # PROVISIONING excludes it.
    precondition_skip = {}
    for t in tests:
        req = t.get("requires")
        if req is None:
            continue
        ok, why = req(args)
        if not ok:
            precondition_skip[t["name"]] = why
    provisionable = [t for t in tests if t["name"] not in precondition_skip]

    plan = None
    if args.local:
        print(
            "provisioning %d local lane set(s) under %s ..."
            % (len(provisionable), make_lane.LANE_ROOT)
        )
        plan = provision_lanes(provisionable, headless=suite["headless"], stock_exe=cfg.stock_exe)
        if plan is None:
            return 1
        suite["timeout_frames"] = LOCAL_TIMEOUT_FRAMES

    jobs = max(1, args.jobs)
    if jobs > 1 and not args.local:
        print(
            "--jobs > 1 requires --local: the VM topology has ONE host VM and one client VM, so two "
            "tests at once would fight over the same two machines. Lanes are what make concurrency "
            "possible, and only --local provisions them."
        )
        return 2
    suite_t0 = time.time()
    rows = run_rows(args, cfg, tests, suite, plan, precondition_skip, jobs)
    counts = print_summary(tests, rows)
    write_suite_timing(args, tests, jobs, suite_t0, counts, rows)
    npass, nfail = counts[0], counts[1]
    if not wanted and not args.solo:
        tp, tf = run_tact_tail(args, cfg, jobs)
        npass, nfail = npass + tp, nfail + tf
    return 1 if nfail else 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    script = old_mode_script(argv)
    if script:
        print(
            "[deprecated] this mode moved to tools/%s.py (same flags); the test_ui.py forward goes "
            "after one cycle (tooling:TL-SUITE-SPLIT)" % script,
            file=sys.stderr,
        )
        return importlib.import_module(script).main(argv, lenient=True)
    ap = build_parser()
    args = ap.parse_args(argv)
    rc = apply_net_args(ap, args, peers_local=args.local)
    if rc:
        return rc
    # Resolved ONCE and passed down explicitly (tooling:TL-SUITE-CONFIG).
    cfg = RunnerConfig.from_args(args)
    print_desktop_banner(cfg)
    if args.check_budgets:
        return check_budgets()
    if args.classify_selftest:
        return classify_selftest()
    if args.loadred_selftest:
        return loadred_selftest()
    if args.walk_watchdog_selftest:
        return walk_watchdog_selftest()
    if args.selftest_refusals:
        return selftest_refusals()
    tests, wanted = select_tests(ap, args)
    if args.list:
        return print_list()
    return run_suite(args, cfg, tests, wanted)


# Offline flags: registry/classifier lints that touch no rig and must not wait for its lease.
OFFLINE_FLAGS = {
    "--check-budgets",
    "--classify-selftest",
    "--loadred-selftest",
    "--walk-watchdog-selftest",  # tooling:TL-HARN19 -- a fake child process, no rig
    "--selftest-refusals",
    "--det-selftest",
    "--list",
}

if __name__ == "__main__":
    import hostlock

    if set(sys.argv[1:]) & OFFLINE_FLAGS:
        raise SystemExit(main())
    raise SystemExit(hostlock.run_rig_tool(main, "test_ui"))
