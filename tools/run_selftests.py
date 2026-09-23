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
import time

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
EXES = tuple(dict.fromkeys(r["exe"] for r in _ROSTER["suites"]))

# The ones that operate big graphs of heap buffers, i.e. whose failure mode includes DYING.
# Repeated on the PLAIN pass, where a crash is the only symptom corruption produces. (See the
# roster's `_flaky` note for why each is in here.)
FLAKY = tuple(_ROSTER["flaky"])


def assert_roster(exe):
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
    r = subprocess.run([exe, "--list-suites"], capture_output=True, text=True, cwd=REPO)
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


def build(asan):
    """Build one mode. Returns {exe name: path}, or None if the build or the staging failed.

    ONE BAT, BOTH PROJECTS. A missing exe is a FAILED BUILD here, not a skipped suite: a driver
    that ran whichever exes it happened to find would report a green pass over half the roster,
    which is the shape every other assertion in this file exists to refuse.
    """
    label = "ASan" if asan else "plain"
    cmd = ["cmd", "/c", BUILD_BAT] + (["--asan"] if asan else [])
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO)
    dt = time.time() - t0
    if r.returncode != 0:
        print(f"[FAIL] build ({label}) exited {r.returncode}")
        print("      " + "\n      ".join((r.stdout + r.stderr).strip().splitlines()[-20:]))
        return None
    out = {}
    for name in EXES:
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


def run_suite(exe, suite, tag):
    """Run one suite once. Returns True on a clean exit. Never parses output for a verdict."""
    r = subprocess.run([exe, suite], capture_output=True, text=True, cwd=REPO)
    if r.returncode == 0:
        return True
    out = (r.stdout + r.stderr).strip()
    # A crash and a failed check are different findings and want different next steps, so say which.
    # Windows returns the status as a large negative int through Python; 0xC0000374 is heap
    # corruption, 0xC0000005 an access violation.
    kind = "CRASH" if (r.returncode < 0 or r.returncode > 255) else "failed check(s)"
    print(f"[FAIL] {tag} {suite}: exit {r.returncode} ({kind})")
    if out:
        print("      " + "\n      ".join(out.splitlines()[:40]))
    elif kind == "CRASH":
        print(
            "      (no output -- stdout is lost on abnormal exit, which is what a crash looks like)"
        )
    return False


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
    args = ap.parse_args()

    ok = True
    t0 = time.time()

    if not args.no_asan and args.which != "plain":
        exes = build(asan=True)
        if exes is None:
            return 1
        # EVERY exe is checked, not just the first: an uninstrumented binary reports no memory
        # errors, which reads exactly like a clean pass, and "the second exe was not instrumented"
        # would be invisible for exactly the spine suites the ASan pass exists for.
        for name, exe in exes.items():
            if not is_instrumented(exe):
                print(f"[FAIL] {exe} is NOT instrumented -- the ASan build staged a plain exe.")
                print("       Refusing to report a clean ASan pass from an uninstrumented binary.")
                return 1
            if not assert_roster(exe):
                return 1
        for suite in SUITES:
            ok &= run_suite(exes[SUITE_EXE[suite]], suite, "asan")
        print(f"[{'ok' if ok else 'FAIL'}] ASan pass ({len(SUITES)} suites, {len(exes)} exes)")

    # The plain pass is a pass in its own right, not cleanup: it is where the flaky-crash repeats
    # run, since a crash is the only symptom corruption produces in an uninstrumented build. (Before
    # 2026-08-23 it was also cleanup -- the modes shared a staging path -- which is no longer true.)
    if args.which != "asan":
        exes = build(asan=False)
        if exes is None:
            return 1
        for exe in exes.values():
            if not assert_roster(exe):
                return 1
        for suite in SUITES:
            reps = args.repeats if suite in FLAKY else 1
            for i in range(reps):
                tag = f"plain[{i + 1}/{reps}]" if reps > 1 else "plain"
                ok &= run_suite(exes[SUITE_EXE[suite]], suite, tag)
        print(
            f"[{'ok' if ok else 'FAIL'}] plain pass ({len(SUITES)} suites across {len(exes)} exes, "
            f"{args.repeats}x {'/'.join(FLAKY)})"
        )

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

    _holder = f"run_selftests:{os.getpid()}"
    _cur = hostlock.held_by("selftest")
    if _cur:
        print(
            f"[selftest] the 'selftest' lease is held by {_cur.get('holder')!r} -- waiting for it "
            "to finish before staging (this run will not start until it is free)..."
        )
    with hostlock.lease("selftest", _holder):
        sys.exit(main())
