"""Run the DLL gate's SELFTEST half (src/mh_dll/README.md step 2) -- under ASan by default.

Why this exists as a script rather than the README's copy-paste block
--------------------------------------------------------------------
Three lessons from the log are encoded here, all of which a hand-run list keeps re-losing:

1. **A selftest can DIE rather than fail, and a crash looks nothing like a FAIL line.** A crashed run
   prints ZERO output (stdout is buffered and lost on abnormal exit), so it does not resemble the
   `2278 checks, 0 failures` pass line -- it resembles *nothing*. Exit codes are the only reliable
   signal, so this driver checks them and never parses output for a verdict.

2. **A single invocation is not evidence for a test that can crash intermittently.** `aitest` and
   `statetest` operate big graphs of heap buffers; both have historically exited 0xC0000374 on a
   large fraction of runs while every once-per-gate run in the log read green (a gate that runs
   each test ONCE cannot see a flaky crash). They are repeated here.

3. **ASan turns that intermittent, non-local corruption into a first-run, deterministic report AT
   the offending instruction.** Measured twice on the same bug class:
     - 2026-08-03, `std::vector<uint32_t> ring_counts{128}` -- ~60% crash rate, found with page heap.
     - 2026-08-06, `std::vector<int32_t> group_scratch{101}` -- 3/20 crash rate; the plain build
       needed 20 runs to look suspicious, ASan named the file and line on run #1.
   Both are the `initializer_list`-instead-of-count trap, which only fires for SCALAR element types
   and which neither clang-tidy nor MSVC /analyze can see (both were run against the 2026-08-03 line
   and came back clean -- the code is legal, unambiguous overload resolution). ASan is what sees it,
   so ASan is the default here rather than a thing to reach for after something already crashed.

Cost (re-measured 2026-08-23, after the build was fixed -- see below): a run that has to build
both modes from scratch is ~165 s, and a run where nothing changed since the last one is ~26 s. The
40 suite invocations are ~24 s of that; everything else is compilation.

TWO THINGS MADE THIS 627 s BEFORE 2026-08-23, and both were in the build rather than here:
  * NO `/MP`. mh_nettest is 669 TUs in ONE project, and MSBuild's `/m` parallelizes across PROJECTS,
    not across a project's source files -- so the whole thing compiled on one core (verified by
    sampling `tasklist` mid-build: exactly one cl.exe). Each build was ~300 s; with
    `<MultiProcessorCompilation>` it is ~70 s.
  * THE TWO MODES SHARED AN IntDir, so each invalidated the other's objects and every run paid two
    FULL rebuilds even when nothing had changed. mh_nettest.vcxproj now splits OutDir/IntDir on
    EnableASAN, which is what makes the ~26 s warm run possible. Do not re-merge them for tidiness.

Usage
-----
    python tools/run_selftests.py              # ASan pass + plain pass  (the gate default)
    python tools/run_selftests.py --no-asan    # plain pass only         (iterating; seconds)
    python tools/run_selftests.py --repeats 20 # widen the flaky-crash net on the plain pass

Each mode has had its OWN build tree since 2026-08-23 (..\\Release\\ vs ..\\Release_asan\\, and
matching IntDirs), so neither leaves the other's exe lying around and neither forces the other to
rebuild. The plain pass still runs last because it is a real pass -- it is where the flaky-crash
repeats live -- not merely to undo the ASan one.

TWO EXES SINCE FORK F5I S2, AND THE OPERATOR INTERFACE DID NOT CHANGE. `net_selftest.exe` is the
HOSTED arm and keeps mh.dll's own machinery; `libmh_selftest.exe` is the STANDALONE arm
(MH_LIBMH_BUILD) and runs the suites that are about libmh itself. Which suite goes where is the
`exe` column of tools/data/selftest_roster.json and nothing in this file -- one build bat builds
both, both are staged into the same directory, both are roster-asserted before anything runs, and
both are checked for ASan instrumentation. The command is still `python tools/run_selftests.py`.
"""

import argparse
import json
import os
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from contextlib import nullcontext
from itertools import groupby

import gate_admission
import tmp_runs

_REPORT_LOCK = threading.Lock()

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_BAT = os.path.join(REPO, "src", "mh_dll", "mh_nettest", "build_selftest.bat")
ROSTER = os.path.join(REPO, "tools", "data", "selftest_roster.json")
TEMP = os.environ.get(
    "TEMP", os.path.join(os.environ.get("USERPROFILE", ""), "AppData", "Local", "Temp")
)
# THE STAGING DIRECTORIES KEEP THEIR NAMES, and that is deliberate rather than an oversight: the
# run contract `%TEMP%\\mh_nettest[_asan]\\` is what build_selftest.bat has always reported,
# tools/lint_machine_paths.py allows by name, and half a dozen hand-run recipes in
# src/mh_dll/README.md paste. F5I added a second EXE to those directories; it did not rename them.
PLAIN_DIR = os.path.join(TEMP, "mh_nettest")
ASAN_DIR = os.path.join(TEMP, "mh_nettest_asan")

# THE ROSTER IS DATA (fork F5I). The gate's suite list used to live here as a hand-maintained
# tuple, and it was the SECOND copy of a list whose first copy is the dispatch table in
# src/mh_dll/mh_nettest/net_selftest.cpp -- with nothing comparing them. A suite could be added to
# the exe and never run by the gate, or sit in this tuple after the exe stopped carrying it, and
# both read as a green run. The per-suite comments that used to sit in this tuple -- the record of
# WHICH DEFECT got each suite added -- moved into the JSON's `why` field with it; they are the most
# valuable thing in the list and were not going to survive as a comment on a deleted tuple.
#
# Three readers, one list: this driver, tools/check_selftest_roster.py (source-side, so the lint
# needs no build) and tools/prove_suite_identity.py.
_ROSTER = json.load(open(ROSTER, encoding="utf-8"))
SUITES = tuple(r["suite"] for r in _ROSTER["suites"])
# suite -> the executable that answers to it, and the exes in roster order. Both derived, so adding
# a third exe is a roster edit and nothing here.
SUITE_EXE = {r["suite"]: r["exe"] for r in _ROSTER["suites"]}
SUITE_CREDITS = {
    r["suite"]: r.get("gate_credits", 6 if r["exe"] == "net_selftest" else 3)
    for r in _ROSTER["suites"]
}
if any(type(c) is not int or c <= 0 for c in SUITE_CREDITS.values()):
    raise ValueError("selftest gate_credits must be positive integer thirds of a core")
EXES = tuple(dict.fromkeys(r["exe"] for r in _ROSTER["suites"]))

# The ones that operate big graphs of heap buffers, i.e. whose failure mode includes DYING.
# Repeated on the PLAIN pass, where a crash is the only symptom corruption produces. (See the
# roster's `_flaky` note for why each is in here.)
FLAKY = tuple(_ROSTER["flaky"])


def assert_roster(exe, env):
    """The exe's OWN suite list must equal the roster's, for this exe, BEFORE anything runs.

    This is the assertion the tuple could never make. `run_suite` checks an exit code, so a suite
    that vanished from the exe cannot be distinguished from one that passed -- the gate would run 32
    names against a binary that only answers to 30 and the two strangers would exit 2 with the mode
    list, which IS caught, and a suite silently ADDED to the exe would never be noticed at all.
    Comparing the live `--list-suites` output closes both directions at the cost of one process
    launch.

    Sets, not sequences: the order lives in the roster file (see its `_order` note) and a reordered
    table is not a defect.
    """
    want = {
        r["suite"]
        for r in _ROSTER["suites"]
        if r["exe"] == os.path.splitext(os.path.basename(exe))[0]
    }
    r = subprocess.run([exe, "--list-suites"], capture_output=True, text=True, cwd=REPO, env=env)
    if r.returncode != 0:
        print(f"[FAIL] {exe} --list-suites exited {r.returncode} -- cannot verify the roster")
        return False
    live = {ln.strip() for ln in r.stdout.splitlines() if ln.strip()}
    if live != want:
        print(f"[FAIL] roster mismatch: exe lists {sorted(live)}, roster lists {sorted(want)}")
        missing, extra = sorted(want - live), sorted(live - want)
        if missing:
            print(f"       in the roster but NOT in the exe: {', '.join(missing)}")
        if extra:
            print(f"       in the exe but NOT in the roster: {', '.join(extra)}")
        print(f"       fix that exe's SUITE_TABLE or {ROSTER} (the map is in")
        print("       tools/check_selftest_roster.py's SOURCES)")
        return False
    return True


# --net-only (CI, user 2026-10-09): libmh is not shipped, so CI builds and runs only the HOSTED arm
# (net_selftest.exe). The local gate keeps both exes.
NET_ONLY = False
NET_EXE = "net_selftest"


def _exes():
    return (NET_EXE,) if NET_ONLY else EXES


def staged(asan):
    """The already-staged exes of one mode (`--no-build`), or None if any is missing.

    THE CALLER OWNS FRESHNESS. `run_gate.py` builds both modes in its serial step 1 (`--build-only`)
    and then runs the suites with `--no-build`, so nothing here can tell a fresh stage from a stale
    one -- which is why this flag is for that pairing and not for a hand run after an edit.
    """
    label = "ASan" if asan else "plain"
    out = {}
    for name in _exes():
        exe = os.path.join(ASAN_DIR if asan else PLAIN_DIR, name + ".exe")
        if not os.path.exists(exe):
            print(f"[FAIL] --no-build: no staged {label} {exe} -- build it first (--build-only)")
            return None
        out[name] = exe
    print(f"[ok] staged ({label}) -- {len(out)} exe(s), not rebuilt (--no-build)")
    return out


def build(asan):
    """Build one mode. Returns {exe name: path}, or None if the build or the staging failed.

    ONE BAT, BOTH PROJECTS. A missing exe is a FAILED BUILD here, not a skipped suite: a driver
    that ran whichever exes it happened to find would report a green pass over half the roster,
    which is the shape every other assertion in this file exists to refuse.
    """
    label = "ASan" if asan else "plain"
    cmd = (
        ["cmd", "/c", BUILD_BAT]
        + (["--asan"] if asan else [])
        + (["--net-only"] if NET_ONLY else [])
    )
    t0 = time.time()
    with gate_admission.lease("selftest:build:" + label, seconds=120, credits=21) as held:
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO, env=held.covered_env())
    dt = time.time() - t0
    if r.returncode != 0:
        print(f"[FAIL] build ({label}) exited {r.returncode}")
        print("      " + "\n      ".join((r.stdout + r.stderr).strip().splitlines()[-20:]))
        return None
    out = {}
    for name in _exes():
        exe = os.path.join(ASAN_DIR if asan else PLAIN_DIR, name + ".exe")
        if not os.path.exists(exe):
            print(f"[FAIL] build ({label}) reported success but {exe} is missing")
            return None
        out[name] = exe
    print(f"[ok] build ({label}) -- {dt:.0f}s, {len(out)} exe(s)")
    return out


def is_instrumented(exe):
    """Is this exe REALLY an ASan build?

    An uninstrumented binary reports no memory errors, which reads EXACTLY like a clean ASan pass --
    so "the ASan step silently did not instrument anything" is the one failure this driver must never
    accept quietly, and the filename is not evidence. Verify the image instead: an instrumented one
    references the ASan runtime by name.

    The two modes no longer share a staging path (2026-08-23), which removes the specific way this
    used to happen -- a no-op ASan build staging the PLAIN exe under the ASan name. The check stays
    anyway: it costs one file read, and the property it asserts is the one the whole ASan pass rests
    on. A guard that is retired because its known trigger was fixed is a guard that was load-bearing
    for the next trigger.
    """
    with open(exe, "rb") as fh:
        return b"clang_rt.asan" in fh.read()


def preflight(exes, *, asan):
    """Check both images under one short lease, released before any suite admission."""
    mode = "asan" if asan else "plain"
    with gate_admission.lease(
        "selftest:preflight:" + mode, seconds=1, credits=3, bootstrap=True
    ) as held:
        env = held.covered_env()
        for exe in exes.values():
            if asan and not is_instrumented(exe):
                print(f"[FAIL] {exe} is NOT instrumented -- the ASan build staged a plain exe.")
                print("       Refusing to report a clean ASan pass from an uninstrumented binary.")
                return False
            if not assert_roster(exe, env):
                return False
    return True


STATE_MASKS = os.path.join(REPO, "tools", "data", "state_masks.json")


def check_state_masks(exe):
    """mp:D42 drift guard: tools/data/state_masks.json must still be what a fresh derivation from
    keep_byte (src/mh_dll/libmh/state/inc_state.h) produces.

    NOT a roster suite. `inchashtest`'s bare gate invocation cannot take this file path -- the exes
    this driver runs are staged into %TEMP%\\mh_nettest[_asan]\\ (TL-STAGE1), so a suite that opened
    a REPO-relative path there would read (or silently miss) the wrong tree. This driver, unlike the
    staged exe, knows REPO, so it calls `inchashtest --check-masks <absolute path>` directly, once,
    against one already-built exe -- the same shape as `assert_roster` above (an extra assertion
    about one exe, not a 33rd/34th suite). Regenerate with:
    `<exe> inchashtest --dump-masks tools/data/state_masks.json`.
    """
    with gate_admission.lease("selftest:state-masks", seconds=1, credits=6) as held:
        r = subprocess.run(
            [exe, "inchashtest", "--check-masks", STATE_MASKS],
            capture_output=True,
            text=True,
            cwd=REPO,
            env=held.covered_env(),
        )
    if r.returncode == 0:
        return True
    out = (r.stdout + r.stderr).strip()
    print(f"[FAIL] state_masks.json drift ({exe} inchashtest --check-masks): exit {r.returncode}")
    if out:
        print("      " + "\n      ".join(out.splitlines()[:20]))
    return False


HEAD_LINES = 40
TAIL_LINES = 3


def failure_report(out):
    """The lines worth showing for a failing suite's output (TL-SELFTEST-TAIL).

    The head alone (the old `splitlines()[:40]`) is the suite's setup chatter on a chatty suite --
    a staterectest red once printed 40 lines of state-record progress and not the check that failed.
    So: the head, then EVERY line past the head that reports a failure (`FAIL`, case-sensitive: the
    suites print `FAIL:`/`[x] FAIL`/`... N failures`), then the last few lines (the summary).
    """
    lines = out.splitlines()
    if len(lines) <= HEAD_LINES + TAIL_LINES:
        return lines
    shown = lines[:HEAD_LINES]
    tail_from = len(lines) - TAIL_LINES
    mid = [
        f"[{i + 1}] {ln}"
        for i, ln in enumerate(lines[HEAD_LINES:tail_from], start=HEAD_LINES)
        if "FAIL" in ln or "failure" in ln or "Assertion" in ln or "ERROR" in ln
    ]
    skipped = (tail_from - HEAD_LINES) - len(mid)
    shown.append(
        f"... {skipped} line(s) omitted; {len(mid)} failure line(s) below, with their line numbers ..."
    )
    return shown + mid + lines[tail_from:]


def suite_credits(suite):
    cfg = gate_admission.configuration()
    price = SUITE_CREDITS[suite]
    # Only an enforced aggregate job allocation licenses the narrower hosted share.
    return min(price, gate_admission.CORE) if cfg is not None and cfg["cpu_bound"] else price


def run_suite(exe, suite, tag, phase=None):
    """Run one suite once. Returns True on a clean exit. Never parses output for a verdict."""
    # Unbounded runs retain roster estimates; bounded roots enforce aggregate active credits.
    credits = suite_credits(suite)
    requested = time.perf_counter()
    admission = (
        gate_admission.lease("selftest:" + tag + ":" + suite, credits=credits)
        if phase is None
        else nullcontext(phase)
    )
    with admission as held:
        started = time.perf_counter()
        r = subprocess.run(
            [exe, suite], capture_output=True, text=True, cwd=REPO, env=held.covered_env()
        )
        elapsed = time.perf_counter() - started
    waited = started - requested
    report = [
        f"[time] {tag} {os.path.basename(exe)} {suite}: {elapsed:.3f}s exit={r.returncode} admission_wait={waited:.3f}s"
    ]
    if r.returncode != 0:
        out = (r.stdout + r.stderr).strip()
        # Keep the complete failing trace; the terminal summary intentionally filters it.
        path = os.path.join(
            tmp_runs.current_output("selftest_failures"),
            tag.replace("/", "_").replace(" ", "_"),
            os.path.basename(exe) + "." + suite + ".log",
        )
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(out)
            report.append("      full failing output: " + os.path.relpath(path, REPO))
        except OSError as exc:
            report.append("      [FAIL] cannot retain full failing output: " + str(exc))
        # A crash and a failed check are different findings; keep the original exit-code verdict.
        kind = "CRASH" if (r.returncode < 0 or r.returncode > 255) else "failed check(s)"
        report.append(f"[FAIL] {tag} {suite}: exit {r.returncode} ({kind})")
        if out:
            report.append("      " + "\n      ".join(failure_report(out)))
        elif kind == "CRASH":
            report.append(
                "      (no output -- stdout is lost on abnormal exit, which is what a crash looks like)"
            )
    elif suite in ("scalefiltertest", "uilayouttest"):
        # These own real device/readback arms with conditional SKIPs. Keep their
        # actual output visible so a constrained run cannot masquerade as exercised coverage.
        report.append((r.stdout + r.stderr).strip())
    with _REPORT_LOCK:
        print("\n".join(report), flush=True)
    return r.returncode == 0


def _transport_lanes(suites):
    return (
        tuple(s for s in ("udploopbacktest", "udpsnaptest") if s in suites),
        tuple(
            s for s in ("meshtest", "udpbulktest", "udprelaytest", "udprelinktest") if s in suites
        ),
    )


def _shared_transport_plan(suites, *, asan, repeats, jobs):
    cfg = gate_admission.configuration()
    if (
        jobs == 1
        or os.environ.get("MH_SELFTEST_PORT_SHIFT", "0") != "0"
        or not cfg
        or not cfg["cpu_bound"]
        or cfg["covered"]
    ):
        return None
    lanes = _transport_lanes(suites)
    if not all(lanes):
        return None
    mode = "asan" if asan else "plain"
    seconds = min(
        86400,
        30 * max(sum(repeats if not asan and s in FLAKY else 1 for s in lane) for lane in lanes),
    )
    return f"selftest:{mode}:network-pair", seconds


def _run_pass(exes, suites, *, asan, repeats, jobs, successors=()):
    """Audited independent lanes within each mode; ordinary shared fixtures stay serial."""

    def run(selected, *, shared=None, following=()):
        ok = True
        # One lease per contiguous same-price serial run, not max-price coverage
        # across heterogeneous suites. This also preserves full-ASan/--exe-jobs1
        # order. Serial-group leases never span joins, barriers or external lock waits;
        # the explicitly shared transport pool instead covers both active socket lanes.
        mode = "asan" if asan else "plain"
        phases = []
        for price, names in groupby(selected, key=suite_credits):
            names = tuple(names)
            invocations = sum(repeats if not asan and s in FLAKY else 1 for s in names)
            phases.append(
                (
                    price,
                    names,
                    f"selftest:{mode}:serial:{names[0]}..{names[-1]}",
                    min(86400, 30 * invocations),
                )
            )
        for i, (price, names, label, seconds) in enumerate(phases):
            requested = time.perf_counter()
            admission = (
                gate_admission.lease(
                    label,
                    credits=price,
                    seconds=seconds,
                    tail=[(p[2], p[3]) for p in phases[i + 1 :]]
                    + list(following)
                    + list(successors),
                )
                if shared is None
                else nullcontext(shared)
            )
            with admission as held:
                with _REPORT_LOCK:
                    print(
                        f"[phase] {mode} {len(names)} suites credits={price} "
                        f"admission_wait={time.perf_counter() - requested:.3f}s",
                        flush=True,
                    )
                for suite in names:
                    count = repeats if not asan and suite in FLAKY else 1
                    for i in range(count):
                        tag = mode if count == 1 else f"plain[{i + 1}/{count}]"
                        ok &= run_suite(exes[SUITE_EXE[suite]], suite, tag, phase=held)
        return ok

    if jobs == 1 or os.environ.get("MH_SELFTEST_PORT_SHIFT", "0") != "0":
        return run(suites)
    if set(exes) - {"net_selftest", "libmh_selftest"}:
        raise ValueError("--exe-jobs 2 requires a resource audit for each additional executable")
    # Loopback's transitive origin/rehome binds overlap snapshot's39800+ ports.
    # The second transport lane uses disjoint39600/39700/40100+ or ephemeral ports.
    udp_a, udp_b = _transport_lanes(suites)
    computational = tuple(s for s in ("inchashtest", "bgscaletest") if s in suites)
    independent = set(udp_a + udp_b + computational)
    if any(SUITE_EXE[s] != "net_selftest" for s in independent):
        raise ValueError("audited independent suite changed executable")
    hosted = tuple(s for s in suites if SUITE_EXE[s] == "net_selftest" and s not in independent)
    memory = tuple(s for s in suites if SUITE_EXE[s] == "libmh_selftest")
    with ThreadPoolExecutor(max_workers=3, thread_name_prefix="selftest-exe") as pool:
        shared = _shared_transport_plan(suites, asan=asan, repeats=repeats, jobs=jobs)
        if shared is not None:
            # Both audited socket lanes need one core each even when their mean
            # CPU use is low: retry/failover deadlines still run in wall time.
            # Memory/ordinary work retains its own allocation; the bulk/uqmatch
            # fence still waits for the entire second transport lane.
            if any(suite_credits(s) != 3 for s in udp_a + udp_b):
                raise ValueError("shared transport pool requires one-core suites")
            label, seconds = shared
            first = pool.submit(run, memory + computational, following=[(label, seconds)])

            def run_networks():
                # Never hold admission while joining another admitted lane:
                # an aged full-capacity reservation must be able to drain us.
                first.result()
                with (
                    gate_admission.lease(
                        label, credits=6, seconds=seconds, tail=successors
                    ) as held,
                    ThreadPoolExecutor(max_workers=2, thread_name_prefix="selftest-net") as net,
                ):
                    a = net.submit(run, udp_a, shared=held)

                    b = net.submit(run, udp_b, shared=held)
                    return all([a.result(), b.result()])

            second = pool.submit(run_networks)
        else:
            second = pool.submit(run, memory + computational + udp_b)
            first = pool.submit(run, udp_a)

        def run_hosted():
            # uqmatchtest and bulk's lossy arm BOTH bind39710..39711. Release
            # admission before waiting; ordinary neighbors and adjacent repeats stay serial.
            split = hosted.index("uqmatchtest") if "uqmatchtest" in hosted else len(hosted)
            ok = run(hosted[:split])
            if split < len(hosted):
                second.result()
                ok &= run(hosted[split:])
            return ok

        ordinary = pool.submit(run_hosted)
        results = [future.result() for future in (first, second, ordinary)]
    return all(results)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--no-asan", action="store_true", help="skip the ASan pass (iterating; seconds)"
    )
    ap.add_argument(
        "--pass",
        dest="which",
        choices=("both", "asan", "plain"),
        default="both",
        help="run only one half. CI runs the halves as two PARALLEL jobs: on a 2-core hosted "
        "runner the ASan build alone measured 936 s (v0.1.0-rc1, 2026-09-18) and the serial gate "
        "blew its 30-minute job budget before the plain build finished. Locally `both` (the gate).",
    )
    ap.add_argument(
        "--repeats",
        type=int,
        default=3,
        help="plain-pass repeats for the crash-prone suites (default 3)",
    )
    ap.add_argument(
        "--exe-jobs",
        type=int,
        choices=(1, 2),
        default=1,
        help="within-mode executable lanes, then audited UDP lanes (default 1); ordinary "
        "hosted work, adjacent repeats, preparation, ASan/plain barriers and outer lease stay serial",
    )
    ap.add_argument(
        "--build-only",
        action="store_true",
        help="build + stage + verify the exes (instrumented, roster), run no suite. run_gate.py's "
        "serial step 1, so no compiler competes with a running game for CPU (2026-09-27).",
    )
    ap.add_argument(
        "--no-build",
        action="store_true",
        help="run the suites against the exes a previous --build-only staged; never rebuilds",
    )
    ap.add_argument(
        "--skip-suites",
        default="",
        help="comma-separated roster suites NOT to run (the light gate's wall-bound loopback "
        "transport suites when no transport source changed -- tooling:TL-GATE12). Each name must "
        "be in the roster; the skipped set is printed, never silent.",
    )
    ap.add_argument(
        "--asan-skip-suites",
        default="",
        help="comma-separated roster suites the ASan pass skips while the plain pass still runs them "
        "(user 2026-10-03, option A: the light gate's wall-bound loopback transport suites on a "
        "transport change -- their ASan coverage stays in the full gate). Printed, never silent.",
    )
    ap.add_argument(
        "--net-only",
        action="store_true",
        help="build and run only net_selftest.exe (the hosted arm); skip libmh_selftest.exe. CI uses it: "
        "libmh is not shipped (user 2026-10-09). Printed, never silent.",
    )
    args = ap.parse_args()
    global NET_ONLY
    NET_ONLY = args.net_only
    asan_skip = {x.strip() for x in args.asan_skip_suites.split(",") if x.strip()}
    unknown_a = sorted(asan_skip - set(SUITES))
    if unknown_a:
        ap.error("--asan-skip-suites names suites not in the roster: %s" % ", ".join(unknown_a))
    if asan_skip:
        print(
            f"[note] --asan-skip-suites: the ASan pass does NOT run {', '.join(sorted(asan_skip))} "
            "(the plain pass does; full gate keeps them under ASan)"
        )
    skip = {x.strip() for x in args.skip_suites.split(",") if x.strip()}
    unknown = sorted(skip - set(SUITES))
    if unknown:
        ap.error("--skip-suites names suites not in the roster: %s" % ", ".join(unknown))
    run_list = tuple(x for x in SUITES if x not in skip)
    if NET_ONLY:
        gone = [x for x in run_list if SUITE_EXE[x] != NET_EXE]
        run_list = tuple(x for x in run_list if SUITE_EXE[x] == NET_EXE)
        print(
            f"[note] --net-only: NOT building/running libmh_selftest.exe ({len(gone)} suites: {', '.join(gone)})"
        )
        asan_skip = {x for x in asan_skip if SUITE_EXE[x] == NET_EXE}
    if skip:
        print(f"[note] --skip-suites: NOT running {', '.join(sorted(skip))} (light gate)")
    if args.build_only and args.no_build:
        ap.error("--build-only and --no-build are exclusive")
    get = staged if args.no_build else build

    ok = True
    t0 = time.time()

    if not args.no_asan and args.which != "plain":
        exes = get(asan=True)
        if exes is None:
            return 1
        if not preflight(exes, asan=True):
            return 1
        if not args.build_only:
            asan_list = tuple(x for x in run_list if x not in asan_skip)
            # The required plain transport chain needs time AFTER the ASan barrier.
            # Forecast that same plan; never overlap the modes or advance a verdict.
            next_mode = (
                _shared_transport_plan(
                    run_list, asan=False, repeats=args.repeats, jobs=args.exe_jobs
                )
                if args.which != "asan"
                else None
            )
            ok &= _run_pass(
                exes,
                asan_list,
                asan=True,
                repeats=1,
                jobs=args.exe_jobs,
                successors=[next_mode] if next_mode is not None else (),
            )
            print(
                f"[{'ok' if ok else 'FAIL'}] ASan pass ({len(asan_list)} suites, {len(exes)} exes)"
            )

    # The plain pass is a pass in its own right, not cleanup: it is where the flaky-crash repeats
    # run, since a crash is the only symptom corruption produces in an uninstrumented build. (Before
    # 2026-08-23 it was also cleanup -- the modes shared a staging path -- which is no longer true.)
    if args.which != "asan":
        exes = get(asan=False)
        if exes is None:
            return 1
        if not preflight(exes, asan=False):
            return 1
        if args.build_only:
            print(f"run_selftests: BUILT in {time.time() - t0:.0f}s (--build-only; no suite ran)")
            return 0
        plain_ok = _run_pass(exes, run_list, asan=False, repeats=args.repeats, jobs=args.exe_jobs)
        # mp:D42. One extra assertion beyond the roster, against the plain exe only (this is a
        # keep_byte-vs-committed-file comparison, not something ASan instrumentation could catch
        # differently) -- see check_state_masks for why it is not a SUITES row.
        if "net_selftest" in exes:
            plain_ok &= check_state_masks(exes["net_selftest"])
        ok &= plain_ok
        print(
            f"[{'ok' if plain_ok else 'FAIL'}] plain pass ({len(run_list)} suites across {len(exes)} exes, "
            f"{args.repeats}x {'/'.join(FLAKY)})"
        )

    if not ok:
        tmp_runs.register(tmp_runs.current_output("selftest_failures"), role="evidence")
    half = "" if args.which == "both" else f" ({args.which} half only)"
    print(f"run_selftests: {'PASS' if ok else 'FAIL'} in {time.time() - t0:.0f}s{half}")
    return 0 if ok else 1


if __name__ == "__main__":
    # TL-STAGE1: the staging dirs (PLAIN_DIR/ASAN_DIR) are the documented run contract
    # (%TEMP%\mh_nettest[_asan]) and are machine-global, not per-invocation -- so two concurrent
    # `run_selftests.py` on one machine (e.g. two worktree gates) stage into the SAME directory and
    # corrupt each other (seen live at mp:T1b, 2026-09-17, plus a same-class fixed-port collision on
    # net_selftest's linktest/relinktest). Serialize with a host-global `selftest` lease -- same shape
    # as the `rig` lease `test_ui.py`/`mp_run.py` take via hostlock.run_rig_tool -- so a second run
    # WAITS for the first instead of racing it. No re-entrancy dance is needed here (unlike `rig`,
    # nothing this script runs spawns a nested run_selftests.py), so a plain `lease()` suffices rather
    # than the fuller run_rig_tool wrapper.
    import hostlock

    gate_admission.relinquish_startup()
    _holder = f"run_selftests:{os.getpid()}"
    _cur = hostlock.held_by("selftest")
    if _cur:
        print(
            f"[selftest] the 'selftest' lease is held by {_cur.get('holder')!r} -- waiting for it "
            "to finish before staging (this run will not start until it is free)..."
        )
    with hostlock.lease("selftest", _holder):
        sys.exit(tmp_runs.run_cli(main, "selftests", adjudicator=True))
