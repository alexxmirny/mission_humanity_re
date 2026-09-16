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
"""

import argparse
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_BAT = os.path.join(REPO, "src", "mh_dll", "mh_nettest", "build_selftest.bat")
TEMP = os.environ.get(
    "TEMP", os.path.join(os.environ.get("USERPROFILE", ""), "AppData", "Local", "Temp")
)
PLAIN_DIR = os.path.join(TEMP, "mh_nettest")
ASAN_DIR = os.path.join(TEMP, "mh_nettest_asan")

# The gate's list, in the README's order. `seamtest` is deliberately ABSENT: it is a KNOWN
# pre-existing failure that crashes on the baseline commit too (verified 2026-07-25 by stashing).
# Adding it here would make the gate permanently red and train everyone to ignore this script.
SUITES = (
    "selftest",
    "selftest3",
    "authtest",
    "linktest",
    "callstest",
    "exportstest",
    # ADDED 2026-08-23 (TACT-PREP). It was omitted, and that omission had teeth: `launchtest`
    # asserts MH_Launch_ParseCmdline's verb ordinals as LITERALS, so inserting a verb into the enum
    # renumbers every later one -- and adding --tactical did exactly that on the first attempt, with
    # nothing in the gate to report it. The test costs milliseconds and guards an append-only
    # contract; there is no reason it was ever outside the suite.
    "launchtest",
    "orderstest",
    # ADDED 2026-08-27 (O4-0). The order-ISSUE oracle: the layer above `orderstest`, proving a
    # reimplemented wrapper packs the SAME order the original packed. Its expectations come from a
    # generated golden, so it grows with the domain rather than with hand-written constants.
    "issuetest",
    "interlocktest",
    # ADDED 2026-09-12 (F1E). The in-memory static-patch applier and its REFUSALS -- a moved guarded
    # byte, an image that already carries the patch, a site inside a promoted body, a cave VA that is
    # taken. Same argument as interlocktest: each is about a write that must NOT happen, so a green
    # rig run cannot stand in for it, and the applier's host table is injected so it touches no real
    # image. Milliseconds.
    "patchtest",
    # ADDED 2026-09-01 (X-TOMB). The tombstone instrument's arming DECISION -- which bodies are
    # filled, over what extent (entry+8 for a promoted body's live redirect, whole for a dead one),
    # and which are correctly skipped. Like interlocktest it is about things NOT happening, so a
    # green rig run cannot stand in for it; the fill is injected, so it touches no real memory.
    "tombstonetest",
    # ADDED 2026-09-02 (LIB-ABI stage B). The host-callback table's BINDING CONTRACT: the
    # version handshake (a mismatched host is refused without clobbering a working binding),
    # the unbound-walk (a missing entry is reported BY NAME), and the trap discipline (a
    # REQUIRED entry reached through the selftest host names itself -- the carrier for the
    # done_when's "no-op'ing a non-notify entry is caught by a test that names it").
    "hostapitest",
    # ADDED 2026-09-11 (LIB-REF-IN). The INBOUND surface's binding contract -- the same three arms
    # as hostapitest in the other direction (handshake, named walk, named trap), and the only place
    # the holed-table states can be reached at all: in a shipped build every inbound entry is an
    # ordinary linked C function, so the one reachable hole is the RUNTIME dispatch table, which
    # only a test seam can punch. Milliseconds, no arena. It must run BEFORE anything binds the
    # regions -- its first arm is the open being refused over an unanswered registry -- which the
    # ordering here happens to give it; if that stops holding the suite says so rather than
    # silently skipping the arm.
    "hostintest",
    "statetest",
    # LIB-BOOT: the post-cfg snapshot import path, its schema guards, the ordering refusal, and
    # the per-block mutation arm that says no carried block is outside the comparison.
    "boottest",
    # LIB-WORLD: the step-0 world fixture -- the format, the refusals, and all THREE coverage arms
    # (hash-slice mutation over the 61 determinism slices, per-block mutation over all 829 blocks,
    # per-block content). ~8 s plain and it earns them: arm B alone re-imports a 7.7 MB blob once
    # per block, which is the only way to say that no carried block is outside the comparison.
    # Under ASan it is slower still and stays in anyway -- this suite mallocs and fills a 7.7 MB
    # arena hundreds of times, which is exactly the shape the ASan pass exists for.
    "worldtest",
    # ADDED 2026-09-11 (LIB-REF). The world blob's FORMAT 2 nav trailer: the map-region
    # decomposition carried as slot indices instead of rebuilt from the `passable` plane, because a
    # rebuild is measurably NOT the recording's partition (2181 of 65536 tiles differ). This suite
    # round-trips a synthetic pool and asserts the reconstruction is SLOT-EXACT -- both list ORDERS,
    # not just membership, because the free list's order decides which node a later region_split
    # gets. The end-to-end proof is the standalone replay, which costs a rig and a fixture; this is
    # the half that costs neither, and it is where every one of the format's refusals is actually
    # fired. Mutation-proven three ways (push-front relink, terrain_flags clobber, free-node
    # neighbour translation), each turning exactly one arm red.
    "navtest",
    # ADDED 2026-09-06 (SB-BIND T1). The state ABI: the host answers where every region lives,
    # through the same C entry a standalone host will use. Shares state_selftest.cpp's TU but is
    # its own suite -- its subject is the BINDING, not the move. Not in FLAKY: its only arena is a
    # 10 KB static, and the registry it touches it puts back.
    "bindtest",
    "aitest",
    "simtest",
    # ADDED 2026-08-24 (TACT-DOMAIN). The OFFLINE half of the tactical oracle: it proves all 14
    # tactical hash slices are actually read and that the manifest lengths match the record strides
    # the disassembly recovered. Not in FLAKY -- its arena is ~880 KB of vectors it only reads and
    # pokes one byte of at a time, not the mutable roster graphs that produced both 0xC0000374s.
    "tacttest",
    # ADDED 2026-09-08 (LIB-CRT). The vendored sprintf family: crt/crt_sprintf.h supplies the 12
    # shapes a standalone libmh cannot reach at a VA, and this says they mean the same thing. The
    # half that earns the suite is the six NARROW sites -- they build filenames ("init\AI03.SCR",
    # "poz3o.dat"), nothing else in the tree compares them, and a wrong one loads a different file
    # instead of misdrawing a pixel. Not in FLAKY: no arena at all, just local buffers.
    "crttest",
    # ADDED 2026-09-08 (CRT-X87 step 2). mh/fp/x87.h's two helpers were inline x87 and are now C++;
    # this keeps a verbatim copy of the assembly as the reference arm and re-proves the equality at
    # BOTH precision settings every run. Its negative arm is load-bearing -- it requires a 53-bit
    # intermediate to still diverge at PC=64, so a sweep that went blind fails instead of passing.
    "fptest",
    "lockstest",
    "resynctest",
    "netsessiontest",
    # ADDED 2026-09-02 (LT0). The lib_trans domain oracle's expectation layer: the RNG-family
    # golden vectors + batch-A wrapper contracts, pinned against the verified rng_next body.
    "libtranstest",
    "watchdogtest",
    # ADDED 2026-08-30 (D21). The runtime desync detector's decisions: what counts as a
    # comparable sample, which regions the verdict drops, and the two arms that must NOT report a
    # desync (a sample from a step we have not reached, and one whose ring entry is gone).
    "desynctest",
    # ADDED 2026-09-02 (D24). Which inbound frame a FULL transport queue may destroy. The defect it
    # guards is silent and luck-dependent -- a peer whose main thread froze lost 15 replicated orders
    # to the old drop-oldest policy -- so a green rig campaign cannot stand in for it.
    "queuetest",
    "savetest",
)

# The ones that operate big graphs of heap buffers, i.e. whose failure mode includes DYING.
# Repeated on the PLAIN pass, where a crash is the only symptom corruption produces. `simtest` joined
# them at SIM0 (2026-08-08): its fixture is ~4 MB of real-extent rosters and the sim writes them, so
# it is in exactly the population that produced both historical 0xC0000374s.
FLAKY = ("aitest", "simtest", "statetest")


def build(asan):
    """Build one mode. Returns the exe path, or None if the build failed."""
    label = "ASan" if asan else "plain"
    cmd = ["cmd", "/c", BUILD_BAT] + (["--asan"] if asan else [])
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO)
    dt = time.time() - t0
    if r.returncode != 0:
        print(f"[FAIL] build ({label}) exited {r.returncode}")
        print("      " + "\n      ".join((r.stdout + r.stderr).strip().splitlines()[-20:]))
        return None
    exe = os.path.join(ASAN_DIR if asan else PLAIN_DIR, "net_selftest.exe")
    if not os.path.exists(exe):
        print(f"[FAIL] build ({label}) reported success but {exe} is missing")
        return None
    print(f"[ok] build ({label}) -- {dt:.0f}s")
    return exe


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
        "--repeats",
        type=int,
        default=3,
        help="plain-pass repeats for the crash-prone suites (default 3)",
    )
    args = ap.parse_args()

    ok = True
    t0 = time.time()

    if not args.no_asan:
        exe = build(asan=True)
        if exe is None:
            return 1
        if not is_instrumented(exe):
            print(f"[FAIL] {exe} is NOT instrumented -- the ASan build staged a plain exe.")
            print("       Refusing to report a clean ASan pass from an uninstrumented binary.")
            return 1
        for suite in SUITES:
            ok &= run_suite(exe, suite, "asan")
        print(f"[{'ok' if ok else 'FAIL'}] ASan pass ({len(SUITES)} suites)")

    # The plain pass is a pass in its own right, not cleanup: it is where the flaky-crash repeats
    # run, since a crash is the only symptom corruption produces in an uninstrumented build. (Before
    # 2026-08-23 it was also cleanup -- the modes shared a staging path -- which is no longer true.)
    exe = build(asan=False)
    if exe is None:
        return 1
    for suite in SUITES:
        reps = args.repeats if suite in FLAKY else 1
        for i in range(reps):
            tag = f"plain[{i + 1}/{reps}]" if reps > 1 else "plain"
            ok &= run_suite(exe, suite, tag)
    print(
        f"[{'ok' if ok else 'FAIL'}] plain pass ({len(SUITES)} suites, {args.repeats}x {'/'.join(FLAKY)})"
    )

    print(f"run_selftests: {'PASS' if ok else 'FAIL'} in {time.time() - t0:.0f}s")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
