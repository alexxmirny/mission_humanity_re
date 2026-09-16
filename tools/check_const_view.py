"""check_const_view.py -- prove the MODULE STATE VIEWS reject illegal writes AT COMPILE TIME.

TWO TARGETS, TWO DIFFERENT PROPERTIES (the second added 2026-08-08 for RI-SIM / SIM0):

  * `mh_nettest/ai_const_negative.cpp` -- AI0's const view. The AI reads the rosters and writes only
    its own island, so P2-RULES' R2 is expressible as pointer-to-const throughout and every case
    fails with an assign-through-const diagnostic.

  * `mh_nettest/sim_write_negative.cpp` -- SIM0's MUTABLE interface. The sim writes the rosters, so
    const-ness cannot carry the rule; sim/sim_state.h states three instead (W1 the read view is
    const and its MEMBERSHIP is the claim, W2 the write store hands out no address, W3 the store
    cannot be constructed outside the module). W2 and W3 are ACCESS-CONTROL properties, so those
    cases must fail with C2248 and NOT with a const diagnostic.

That last point is why this driver grew a per-case expectation instead of one accept-set: a case
that fails with the OTHER rule's diagnostic is a broken test, and a single wide accept-set would
report it green. The expectation is read out of the source -- each `#if defined(MH_..._CASE_n)` line
carries a trailing `// EXPECT const` or `// EXPECT access` -- so a new case declares its own class in
the one place a reader is already looking, and the driver has no table to forget to update.

WHAT IT RUNS.  Each target is one translation unit carrying N mutually-exclusive cases behind
`MH_..._CASE_<n>`.  For each case the driver compiles the TU with that macro defined and requires
the compile to FAIL **with that case's expected diagnostic**; then it compiles with NO macro at all
and requires that to SUCCEED.

WHY BOTH ARMS.  A negative test that only asks "did the compiler say no?" is satisfied by a typo, a
missing include, or a field this repo renamed last week -- every one of which looks exactly like the
architecture rule holding while proving nothing about it. So the positive arm must compile, which is
what says the file is otherwise sound; if it broke, every negative case would keep "passing" for the
wrong reason.

That pairing is this check's own mutation test: delete a `const` from ai_view, or make sim_store's
members public, and the affected cases go green-to-red the right way round (verify by hand with
--verbose if you ever doubt it).

Usage:
  python tools/check_const_view.py             # the gate: exit 0 iff every case behaves
  python tools/check_const_view.py --verbose   # print each compiler invocation and its diagnostic
  python tools/check_const_view.py --target sim   # just one target (ai | sim)
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import machine_config as machine  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NETTEST = os.path.join(REPO, "src", "mh_dll", "mh_nettest")
INCLUDE_DIRS = [
    # F5O: the roster is under libmh/ now; mh/ still carries addr/, crt/, fp/, include/.
    os.path.join(REPO, "src", "mh_dll", "libmh"),
    os.path.join(REPO, "src", "mh_dll", "mh"),
    os.path.join(REPO, "src", "mh_dll"),
]

# The MSVC diagnostics that mean "you tried to write through a const".  Anything else -- a missing
# header, an unknown identifier, a renamed field -- is NOT this test passing, it is this test being
# broken, and is reported as such.
# DELIBERATELY NARROW.  C2440 (cannot convert) and C2100 (illegal indirection) would also fire on a
# genuinely broken test, so they are NOT here: a check whose accept-set is wide enough to swallow its
# own breakage is the failure mode this file exists to avoid.
CONST_DIAGNOSTICS = re.compile(
    r"error C(3490|3892|2166|2678|2679)\b"  # the assign-through-const family
    r"|cannot be modified because it is being accessed through a const object"
    r"|cannot assign to a variable that is const"
    r"|l-value specifies const object",
    re.IGNORECASE,
)

# The access-control family: SIM0's W2/W3.  C2248 is "cannot access <access> member declared in
# class"; C2664/C2440 are deliberately absent for the same reason as above.  C2512 covers "no
# appropriate default constructor available", which is how some MSVC versions phrase an
# inaccessible default ctor reached through aggregate initialization.
ACCESS_DIAGNOSTICS = re.compile(
    r"error C(2248|2512)\b" r"|cannot access private member" r"|cannot access .*member declared in",
    re.IGNORECASE,
)

DIAGNOSTIC_CLASSES = {"const": CONST_DIAGNOSTICS, "access": ACCESS_DIAGNOSTICS}

TARGETS = [
    {
        "name": "ai",
        "tu": os.path.join(NETTEST, "ai_const_negative.cpp"),
        "macro": "MH_CONST_NEGATIVE_CASE_%d",
        "case_re": re.compile(r"MH_CONST_NEGATIVE_CASE_(\d+)\)(?:\s*//\s*EXPECT\s+(\w+))?"),
        "banner": "AI0: a write through ai_view must not compile",
        "positive": "legal reads + writes through ai_store",
    },
    {
        "name": "sim",
        "tu": os.path.join(NETTEST, "sim_write_negative.cpp"),
        "macro": "MH_SIM_NEGATIVE_CASE_%d",
        "case_re": re.compile(r"MH_SIM_NEGATIVE_CASE_(\d+)\)(?:\s*//\s*EXPECT\s+(\w+))?"),
        "banner": "SIM0: W1 const view / W2 no escaping address / W3 no forged store",
        "positive": "legal reads + writes through sim_store's accessors",
    },
    {
        "name": "tact",
        "tu": os.path.join(NETTEST, "tact_write_negative.cpp"),
        "macro": "MH_TACT_NEGATIVE_CASE_%d",
        "case_re": re.compile(r"MH_TACT_NEGATIVE_CASE_(\d+)\)(?:\s*//\s*EXPECT\s+(\w+))?"),
        "banner": "TACT0: W1 const view (strategic state is read-only to a mission) / W2 / W3, "
        "plus the SHARED PLANES, which must be forgeable by nobody",
        "positive": "legal reads through tact_view + writes through tact_store and mode_planes",
    },
]


def find_vcvars():
    """Locate `vcvars32.bat`, NOT cl.exe directly.

    cl.exe run without the toolchain environment cannot find `<cstdint>` and fails with C1083 --
    which is a compile failure, and would therefore have made every negative case below "pass" for
    entirely the wrong reason if the diagnostic were not checked.  That is not hypothetical: it is
    what this driver did on its first run.
    """
    vswhere = os.path.join(
        os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
        "Microsoft Visual Studio",
        "Installer",
        "vswhere.exe",
    )
    roots = []
    if os.path.isfile(vswhere):
        try:
            out = subprocess.run(
                [vswhere, "-latest", "-products", "*", "-property", "installationPath"],
                capture_output=True,
                text=True,
                timeout=30,
            ).stdout.strip()
            roots += [ln.strip() for ln in out.splitlines() if ln.strip()]
        except Exception:
            pass
    roots.append(machine.VS_INSTALL_ROOT)
    for root in roots:
        bat = os.path.join(root, "VC", "Auxiliary", "Build", "vcvars32.bat")
        if os.path.isfile(bat):
            return bat
    hits = sorted(
        glob.glob(r"C:\Program Files*\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars32.bat")
    )
    return hits[-1] if hits else None


def cases_in(target):
    """The case numbers the TU defines and the diagnostic class each declares, read from the source
    rather than hardcoded -- so adding a case adds it to the gate with no second edit to remember.

    A case with no `// EXPECT <class>` marker defaults to `const`, which is what every AI0 case is.
    An UNKNOWN class is an error rather than a default: a typo in the marker would otherwise silently
    widen the accept-set, which is the exact failure this driver exists to avoid.
    """
    src = open(target["tu"], encoding="utf-8").read()
    out = {}
    for num, cls in target["case_re"].findall(src):
        out[int(num)] = (cls or "const").lower()
    return dict(sorted(out.items()))


def compile_once(vcvars, tu, macro, workdir, verbose):
    cl = ["cl", "/nologo", "/c", "/std:c++20", "/EHsc", "/permissive-", "/W3"]
    for d in INCLUDE_DIRS:
        cl += ["/I", '"%s"' % d]
    if macro:
        cl.append("/D" + macro)
    cl += ['/Fo"%s"' % os.path.join(workdir, "out.obj"), '"%s"' % tu]
    bat = os.path.join(workdir, "run.bat")
    with open(bat, "w", encoding="ascii") as f:
        f.write('@echo off\ncall "%s" >nul\n%s\n' % (vcvars, " ".join(cl)))
    if verbose:
        print("   $", " ".join(cl))
    r = subprocess.run(["cmd", "/c", bat], capture_output=True, text=True, cwd=workdir, timeout=300)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def run_target(target, vcvars, verbose):
    """Returns (n_cases, n_failures).  A structural problem returns (0, 1) so it can never read as
    'zero cases, zero failures, fine'."""
    print("=== check_const_view [%s] (%s) ===" % (target["name"], target["banner"]))
    if not os.path.isfile(target["tu"]):
        print("  FAIL -- missing %s" % target["tu"])
        return 0, 1

    cases = cases_in(target)
    if not cases:
        print("  FAIL -- the TU declares no cases")
        return 0, 1
    unknown = {n: c for n, c in cases.items() if c not in DIAGNOSTIC_CLASSES}
    if unknown:
        print(
            "  FAIL -- unknown EXPECT class(es): %r (known: %s)"
            % (unknown, list(DIAGNOSTIC_CLASSES))
        )
        return 0, 1

    failures = 0
    with tempfile.TemporaryDirectory(prefix="mh_stateview_") as wd:
        # THE POSITIVE ARM FIRST.  If the file does not compile clean, every negative result below
        # is uninterpretable, so this is a hard stop rather than one more failure line.
        rc, out = compile_once(vcvars, target["tu"], None, wd, verbose)
        if rc != 0:
            print("  FAIL: the POSITIVE arm does not compile, so no negative case means anything.")
            print("        Every legal read/write in the #else branch must build.")
            print(out.strip()[:4000])
            return len(cases), len(cases)
        print("  ok: positive arm compiles (%s)" % target["positive"])

        # The cases are N INDEPENDENT compiles of the same TU under mutually-exclusive macros, and
        # each one pays a full `vcvars32.bat` before cl even starts -- which is most of the ~1.6 s a
        # case costs. Fanning them out took this check from 19.5 s to ~4 s, the single largest lane
        # in lint_repo. Each gets its OWN workdir: compile_once writes `run.bat` and `out.obj` by
        # fixed name, so sharing one would have the cases overwrite each other's inputs.
        def compile_case(n):
            cwd = os.path.join(wd, "case%d" % n)
            os.makedirs(cwd, exist_ok=True)
            return compile_once(vcvars, target["tu"], target["macro"] % n, cwd, verbose)

        with ThreadPoolExecutor(max_workers=min(len(cases), (os.cpu_count() or 4))) as ex:
            results = dict(zip(cases, ex.map(compile_case, cases)))

        # Reported in CASE order regardless of completion order -- this output is read as a list.
        for n, cls in cases.items():
            rc, out = results[n]
            if rc == 0:
                print("  FAIL: case %d COMPILED -- the interface accepted what it must reject." % n)
                failures += 1
            elif not DIAGNOSTIC_CLASSES[cls].search(out):
                print("  FAIL: case %d failed, but NOT with a '%s' diagnostic." % (n, cls))
                print("        That is a broken test, not a passing one. Compiler said:")
                for line in out.strip().splitlines():
                    if "error" in line.lower():
                        print("          " + line.strip())
                failures += 1
            else:
                first = next(
                    (ln.strip() for ln in out.splitlines() if "error" in ln.lower()),
                    "(no error line)",
                )
                print("  ok: case %d rejected [%s] -- %s" % (n, cls, first[:130]))

    print("  %d case(s), %d failure(s)" % (len(cases), failures))
    return len(cases), failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--target", choices=[t["name"] for t in TARGETS], help="run only this target")
    args = ap.parse_args()

    vcvars = find_vcvars()
    if vcvars is None:
        print(
            "check_const_view: vcvars32.bat not found -- SKIPPED (this check needs the MSVC toolchain)"
        )
        return 0
    print("toolchain: %s" % vcvars)

    targets = [t for t in TARGETS if args.target is None or t["name"] == args.target]
    total_cases = 0
    total_fail = 0
    for t in targets:
        n, f = run_target(t, vcvars, args.verbose)
        total_cases += n
        total_fail += f

    print(
        "TOTAL: %d case(s) over %d target(s), %d failure(s)"
        % (total_cases, len(targets), total_fail)
    )
    return 1 if total_fail else 0


if __name__ == "__main__":
    sys.exit(main())
