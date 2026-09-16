"""prove_suite_identity.py -- fork F5I: the selftest exe's OBSERVABLE OUTPUT is a baseline.

WHY THIS EXISTS. F5I moves 340 selftest translation units into their own tree and then splits the
one selftest executable into two. Every step of that is supposed to be behaviour-neutral, and
"behaviour-neutral" is exactly the claim a gate cannot make for itself: `run_selftests.py` asserts
each suite EXITS 0, which it did before the move and will do after it, whether or not the suite is
still running the same checks. A suite that silently stopped registering half its assertions exits 0
and prints a smaller number, and nothing in the tree reads that number.

So this records the number, and the whole transcript around it. `--record` captures every gate
suite's normalised stdout+stderr and exit code from a KNOWN-GOOD exe; `--check` re-runs them against
a candidate exe and refuses any difference. The unit of proof is per-suite text equality, which
catches the three things an exit code cannot: a check count that moved, an arm that stopped
printing, and an arm that started printing something new.

WHAT IS NORMALISED, AND WHY SO LITTLE. Two full record passes over the same exe were diffed to find
what actually varies run to run (tmp/f5i/explore/, 2026-09-16). TWO tokens do:  # CITATION-OK

  1. linktest: "[watch]  dropped the silent peer after 2140 ms"  ->  "... after 2109 ms"
     The drop latency of a real socket watchdog. The suite's verdict is the drop, not its latency.

  2. worldtest: "  capture: ... ptrs head=0 span=21"  ->  "... ptrs head=0 span=7004"
     The interior-pointer census of a SYNTHESISED arena -- how many words of freshly malloc'd,
     never-written fixture memory happen to look like pointers into the registry. Over 10 runs it
     took three distinct values (0, 21, 7004). It is a `printf` diagnostic, not a `ck()` assertion,
     so nothing checkable is lost by stripping it. `head` is stripped alongside `span` even though
     it read 0 in all ten runs: it is the same class of quantity counted over the same garbage, and
     pinning it would only mean the baseline goes flaky the first time the allocator hands back a
     different page.

AND THE LESSON IN #2 IS THE PROJECT'S OWN. Two record passes did NOT find it -- both drew the same
value, and the pattern only surfaced when a third run (the same build from a different directory)
disagreed. Same shape as the ASan argument in run_selftests.py: one clean pass of something that
varies intermittently is not evidence. Record and check a NEW roster suite several times before
trusting its first green.

The volatile-ROOT substitutions (the exe's own directory, the repo root, %TEMP%) are there for a
different reason -- they do not vary between two runs of one exe, they vary when the SAME exe is run
from a different location, which is precisely what `--check` does when it compares
`%TEMP%\\mh_nettest\\net_selftest.exe` against a baseline recorded from a copy.

WHAT IS DELIBERATELY *NOT* NORMALISED: hex. A generic `0x[0-9a-f]{6,}` strip was considered and
rejected on the evidence -- the only 6+-digit hex these transcripts print is load-bearing and
stable: the original's instruction addresses that tacttest cites in its arm names (`@0x0043a1fa`),
and the two host-ABI version hashes hostapitest and hostintest print (`0xFD4BC3F8`, `0x8777535E`).
Stripping those would blind the baseline to an ABI version change and to a retargeted arm, which are
two of the things it is here to see. A real heap pointer has never appeared in a green transcript;
if one ever does it will read as DIFFERS, which is the correct answer rather than a missed one.

THE BASELINE IS NOT COMMITTED. Transcripts land in tmp/f5i/baseline/ (gitignored) because they are  # CITATION-OK
an in-flight comparison between two commits, not a durable contract -- the durable half is the check
COUNT per suite, which F5I commits to tools/data/selftest_roster.json.

TWO EXES SINCE F5I S2, AND THE BASELINE DOES NOT KNOW IT. That is the point: the baseline was
recorded from the ONE hosted exe before the split, so re-running a moved suite on
`libmh_selftest.exe` and getting the same transcript is the measurement that says the arm change
(HOSTED -> STANDALONE) changed nothing observable. So `--exe` may now be given more than once, or
`--exe-dir` may name the staging directory that holds both, and each suite is routed to the exe
tools/data/selftest_roster.json names for it. The single `--exe` form still works and still means
"run these suites on THIS binary" -- with `--suites` that is how the same 18 names are re-run on the
old exe, which is what isolates "the new arm" from "a new build".

Usage:
  python tools/prove_suite_identity.py --list
  python tools/prove_suite_identity.py --record --exe-dir %TEMP%\\mh_nettest
  python tools/prove_suite_identity.py --check  --exe-dir %TEMP%\\mh_nettest
  python tools/prove_suite_identity.py --check  --exe <net_selftest.exe> --exe <libmh_selftest.exe>
  python tools/prove_suite_identity.py --check  --exe <one exe> --suites aitest,simtest
  python tools/prove_suite_identity.py --selftest   # prove --check can go red
"""

import argparse
import difflib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

import run_selftests  # noqa: E402  -- one roster, read from the driver that owns it

DEFAULT_BASELINE = os.path.join(REPO, "tmp", "f5i", "baseline")
TEMP = os.environ.get(
    "TEMP", os.path.join(os.environ.get("USERPROFILE", ""), "AppData", "Local", "Temp")
)

# The measured run-to-run variance (see the header). Two patterns, two suites.
RE_VOLATILE = (
    (re.compile(r"\b\d+ ms\b"), "<ms> ms"),  # linktest: socket-watchdog drop latency
    # worldtest: pointer-shaped garbage in a freshly malloc'd fixture arena
    (re.compile(r"\bptrs head=\d+ span=\d+"), "ptrs head=<n> span=<n>"),
)
# The `N checks, M failures` pass line. Not every suite prints one -- the seven transport/orchestrator
# suites report by exit code and prose instead -- so a missing line is recorded as null, never as 0.
RE_CHECKS = re.compile(r"\b(\d+) checks?, (\d+) failures?\b")


def suites():
    """The gate roster, in the driver's order."""
    return list(run_selftests.SUITES)


def resolve_exes(exe_args, exe_dir, names):
    """{suite: exe path} for `names`, or (None, message) on a routing failure.

    THREE FORMS, ONE RULE. `--exe-dir` takes the roster's exe column as filenames in that directory;
    repeated `--exe` matches each path to the roster exe of the same basename; a SINGLE `--exe` is
    the escape hatch and routes every named suite to it regardless of the roster, which is what lets
    the old exe be re-measured on suites that have moved to the new one.
    """
    want = {run_selftests.SUITE_EXE[s] for s in names}
    if exe_dir:
        out = {}
        for s in names:
            path = os.path.join(exe_dir, run_selftests.SUITE_EXE[s] + ".exe")
            if not os.path.exists(path):
                return None, "no such exe: %s (needed by %s)" % (path, s)
            out[s] = os.path.abspath(path)
        return out, None
    if len(exe_args) == 1:
        return {s: os.path.abspath(exe_args[0]) for s in names}, None
    by_name = {}
    for e in exe_args:
        by_name[os.path.splitext(os.path.basename(e))[0]] = os.path.abspath(e)
    missing = sorted(want - set(by_name))
    if missing:
        return None, "no --exe given for %s (the roster routes %d of the named suites there)" % (
            ", ".join(missing),
            len([s for s in names if run_selftests.SUITE_EXE[s] in missing]),
        )
    return {s: by_name[run_selftests.SUITE_EXE[s]] for s in names}, None


def normalise(text, exe):
    """Strip the tokens that are a property of WHERE and WHEN the exe ran, not of what it did."""
    out = text.replace("\r\n", "\n")
    # Longest first: the exe path contains its directory, which may contain TEMP.
    roots = [
        (os.path.abspath(exe), "<EXE>"),
        (os.path.dirname(os.path.abspath(exe)), "<EXEDIR>"),
        (os.path.abspath(REPO), "<REPO>"),
        (os.path.abspath(TEMP), "<TEMP>"),
    ]
    for path, token in sorted(roots, key=lambda r: -len(r[0])):
        if not path:
            continue
        for spelling in (path, path.replace("\\", "/"), path.replace("/", "\\")):
            out = re.sub(re.escape(spelling), token, out, flags=re.IGNORECASE)
    for rx, token in RE_VOLATILE:
        out = rx.sub(token, out)
    return out


def run_one(exe, suite):
    """Run one suite bare, as run_selftests.py runs it. Returns (normalised text, exit code).

    savetest / boottest / worldtest accept an optional argv[2] fixture; the gate runs them WITHOUT
    one (the synthesised-fixture arm), so this does too -- a baseline recorded over a different
    invocation than the gate's would prove identity of something the gate never runs.
    """
    r = subprocess.run([exe, suite], capture_output=True, text=True, cwd=REPO)
    return normalise(r.stdout + r.stderr, exe), r.returncode


def parse_checks(text):
    """Sum the `N checks, M failures` lines. None when the suite prints none.

    SUM, not last: `crttest` is two halves run in one invocation (the sprintf family and the five
    other vendored headers) and prints a line for each, so the last line is only half the suite.
    """
    hits = RE_CHECKS.findall(text)
    if not hits:
        return None, None
    return sum(int(c) for c, _ in hits), sum(int(f) for _, f in hits)


def record(routes, names, baseline, whole_roster):
    # A NARROWED --record MERGES into the existing baseline; only a whole-roster record wipes it.
    # The first version wiped unconditionally, and `--record --suites fptest` (re-recording the one
    # suite whose transcript had legitimately changed) silently threw away the other 31 baselines --
    # the next --check then read "1/32 identical" and the whole hosted-arm provenance was gone.
    # Measured 2026-09-16 by the conductor, the same session the tool was written.
    summary = None
    if os.path.isdir(baseline) and not whole_roster:
        sp = os.path.join(baseline, "summary.json")
        if os.path.isfile(sp):
            with open(sp, encoding="utf-8") as fh:
                summary = json.load(fh)
    elif os.path.isdir(baseline):
        shutil.rmtree(baseline)
    os.makedirs(baseline, exist_ok=True)
    if summary is None:
        summary = {
            "_what": "fork F5I identity baseline: per-suite normalised transcript + exit code.",
            "exe": [],
            "suites": {},
        }
    summary["exe"] = sorted(set(summary.get("exe", [])) | {routes[s] for s in names})
    for s in names:
        exe = routes[s]
        text, code = run_one(exe, s)
        with open(os.path.join(baseline, s + ".txt"), "w", newline="\n", encoding="utf-8") as fh:
            fh.write(text)
        with open(os.path.join(baseline, s + ".exit"), "w", newline="\n", encoding="utf-8") as fh:
            fh.write(str(code))
        checks, failures = parse_checks(text)
        summary["suites"][s] = {"exit": code, "checks": checks, "failures": failures}
        print(
            "  recorded %-16s exit=%d checks=%s failures=%s"
            % (s, code, "-" if checks is None else checks, "-" if failures is None else failures)
        )
    with open(os.path.join(baseline, "summary.json"), "w", newline="\n", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=2)
        fh.write("\n")
    missing = [s for s, v in summary["suites"].items() if v["checks"] is None]
    print("=== recorded %d suite(s) into %s ===" % (len(names), baseline))
    if missing:
        print(
            "  no `N checks, M failures` line (identity = exit code + text only): %s"
            % ", ".join(missing)
        )
    return 0


def check(routes, names, baseline):
    if not os.path.isdir(baseline):
        print("[FAIL] no baseline at %s -- run --record against a known-good exe first" % baseline)
        return 1
    bad = 0
    # Name the exe per row only when the run actually spans more than one -- a single-exe run would
    # otherwise repeat the same basename 32 times.
    multi = len({routes[s] for s in names}) > 1
    for s in names:
        want_txt = os.path.join(baseline, s + ".txt")
        want_exit = os.path.join(baseline, s + ".exit")
        if not (os.path.exists(want_txt) and os.path.exists(want_exit)):
            print("[FAIL] %-16s MISSING from the baseline" % s)
            bad += 1
            continue
        with open(want_txt, encoding="utf-8") as fh:
            want = fh.read()
        with open(want_exit, encoding="utf-8") as fh:
            want_code = int(fh.read().strip())
        got, code = run_one(routes[s], s)
        if got == want and code == want_code:
            print(
                "  IDENTICAL %-16s exit=%d   %s"
                % (s, code, os.path.basename(routes[s]) if multi else "")
            )
            continue
        bad += 1
        print(
            "[FAIL] %-16s DIFFERS (exit %d, baseline %d)%s"
            % (s, code, want_code, "  " + os.path.basename(routes[s]) if multi else "")
        )
        d = list(
            difflib.unified_diff(
                want.splitlines(), got.splitlines(), "baseline", "candidate", lineterm="", n=1
            )
        )
        for line in d[:24]:
            print("      %s" % line)
        if len(d) > 24:
            print("      ... %d more diff line(s)" % (len(d) - 24))
    # A baseline suite the candidate was not asked about is also a difference -- say so rather than
    # letting a narrowed --suites run read as a whole-roster pass.
    extra = sorted(
        f[:-4] for f in os.listdir(baseline) if f.endswith(".txt") and f[:-4] not in names
    )
    if extra:
        print("  (not checked this run: %s)" % ", ".join(extra))
    print(
        "prove_suite_identity: %s -- %d/%d identical"
        % ("PASS" if not bad else "FAIL", len(names) - bad, len(names))
    )
    return 1 if bad else 0


def selftest(exe, baseline):
    """Prove --check can go red. Plants each defect class in a COPY of the baseline directory.

    A comparison tool that has never been shown to fail is the same liability as a suite that cannot
    go red -- which is the defect F5I's whole step plan exists to rule out.
    """
    if not os.path.isdir(baseline):
        print("[FAIL] --selftest needs an existing baseline at %s" % baseline)
        return 1
    if not exe or not os.path.exists(exe):
        print("[FAIL] --selftest needs --exe (the exe the baseline was recorded from)")
        return 1
    # The victim must be a suite THIS exe answers to: --selftest is handed one binary, and a
    # routed suite from the other one would exit 2 with the mode list and go red for the wrong
    # reason.
    stem = os.path.splitext(os.path.basename(exe))[0]
    names = [s for s in suites() if run_selftests.SUITE_EXE[s] == stem] or suites()
    victim = names[0]
    fails = 0
    tmpdir = tempfile.mkdtemp(prefix="f5i_identity_")
    try:
        # (a) a mutated transcript -- the "a check count moved / an arm went quiet" class
        planted = os.path.join(tmpdir, "text")
        shutil.copytree(baseline, planted)
        with open(os.path.join(planted, victim + ".txt"), "a", encoding="utf-8") as fh:
            fh.write("PLANTED: an arm that was not in the baseline\n")
        if check({victim: exe}, [victim], planted) == 0:
            print("  [FAIL] a mutated baseline transcript did NOT go red")
            fails += 1
        else:
            print("  ok: a mutated transcript is caught")

        # (b) a wrong exit code
        planted = os.path.join(tmpdir, "exit")
        shutil.copytree(baseline, planted)
        with open(os.path.join(planted, victim + ".exit"), "w", encoding="utf-8") as fh:
            fh.write("7")
        if check({victim: exe}, [victim], planted) == 0:
            print("  [FAIL] a wrong baseline exit code did NOT go red")
            fails += 1
        else:
            print("  ok: an exit-code difference is caught")

        # (c) a suite the baseline does not carry -- the "a suite vanished from the exe" class,
        #     which is the one a pure text diff over whatever files happen to exist cannot see.
        planted = os.path.join(tmpdir, "missing")
        shutil.copytree(baseline, planted)
        os.remove(os.path.join(planted, victim + ".txt"))
        if check({victim: exe}, [victim], planted) == 0:
            print("  [FAIL] a missing baseline suite did NOT go red")
            fails += 1
        else:
            print("  ok: a missing suite is caught")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    print("=== prove_suite_identity --selftest: %d failure(s) ===" % fails)
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--record", action="store_true", help="capture the baseline from --exe")
    ap.add_argument(
        "--check", action="store_true", help="re-run --exe and diff against the baseline"
    )
    ap.add_argument("--list", action="store_true", help="print the gate roster")
    ap.add_argument("--selftest", action="store_true", help="prove --check can go red")
    ap.add_argument(
        "--exe",
        action="append",
        default=[],
        help="a selftest exe; repeat it to give one per roster exe. A single --exe runs every "
        "named suite on that binary regardless of the roster.",
    )
    ap.add_argument(
        "--exe-dir",
        help="the staging directory holding all of them (e.g. %%TEMP%%\\mh_nettest); each suite is "
        "routed to <dir>\\<the roster's exe for it>.exe",
    )
    ap.add_argument("--suites", help="comma-separated subset (default: the whole roster)")
    ap.add_argument("--baseline", default=DEFAULT_BASELINE, help="baseline directory")
    args = ap.parse_args()

    names = suites()
    if args.suites:
        want = [s.strip() for s in args.suites.split(",") if s.strip()]
        unknown = [s for s in want if s not in names]
        if unknown:
            print("[FAIL] not gate suites: %s" % ", ".join(unknown))
            return 2
        names = want

    if args.list:
        for s in names:
            print(s)
        return 0
    if not args.exe and not args.exe_dir:
        print("[FAIL] --exe (or --exe-dir) is required for --record/--check/--selftest")
        return 2
    for e in args.exe:
        if not os.path.exists(e):
            print("[FAIL] no such exe: %s" % e)
            return 2
    if args.exe_dir and not os.path.isdir(args.exe_dir):
        print("[FAIL] no such directory: %s" % args.exe_dir)
        return 2
    if args.selftest:
        # One binary, one arm: --selftest plants defects in a COPY of the baseline and needs a
        # single exe it can re-run. Paths are absolutised here because every suite is launched with
        # cwd=REPO and a relative exe would resolve against that, not the caller's shell.
        if len(args.exe) != 1:
            print("[FAIL] --selftest takes exactly one --exe")
            return 2
        return selftest(os.path.abspath(args.exe[0]), args.baseline)
    routes, err = resolve_exes(args.exe, args.exe_dir, names)
    if routes is None:
        print("[FAIL] %s" % err)
        return 2
    if args.record:
        return record(routes, names, args.baseline, whole_roster=not args.suites)
    if args.check:
        return check(routes, names, args.baseline)
    print("[FAIL] one of --record / --check / --list / --selftest is required")
    return 2


if __name__ == "__main__":
    sys.exit(main())
