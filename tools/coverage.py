#!/usr/bin/env python3
"""coverage.py -- EXECUTION-LANE coverage for our C++ bodies, via OpenCppCoverage.

WHAT THIS ANSWERS THAT NOTHING ELSE DOES. `journal_coverage.py` says which COMMANDS a recorded
session issues. The migration ledger says which bodies EXIST. Neither says which lines of ours a run
actually executed -- and on 2026-09-04 that gap cost a wrong conclusion twice in one session: a
seeded defect in llm_tact_unit_stand_tick was invisible to every gate because in ship config our
stand_tick body never runs at all (the original tick chain calls the original). "Present in the
binary" and "executed by this scenario" are different facts, and until this tool there was no
mechanical way to tell them apart short of hand-inserting a printf.

THE TRAP THIS TOOL REFUSES TO WALK INTO, measured rather than assumed. Our Release build is /O2 with
WholeProgramOptimization, which inlines the small translated bodies out of existence. Coverage
collected against it reports lines that DID run as never hit:

    Release /O2+LTCG   tact_unit_stand_tick.cpp    0/20    0%   <- FALSE. Its T1-T6 checks all pass.
    Debug   (no opt)   tact_unit_stand_tick.cpp   12/27   44%   <- the real figure

A 0% that means "inlined away" is indistinguishable from a 0% that means "dead code you should
delete", and acting on the first reading is how a correct body gets removed. So this tool builds and
measures the DEBUG configuration, and refuses a Release binary unless forced.

WHAT THE DENOMINATOR ACTUALLY IS, because "50% of 61,186 lines" invites the wrong reading. The
totals are cobertura <line> elements, which OpenCppCoverage emits from the PDB LINE TABLE -- one entry
per line that GENERATED CODE. Comments, blank lines and pure declarations are not in it. Measured
against the real sources (2026-09-08):

    harness.cpp             1,779 instrumented / 7,707 raw = 23%
    sim_state.cpp             623 / 1,730 = 36%
    sim_order_enqueue.cpp     509 /   950 = 54%      ai_abandon_target   45 /   155 = 29%

Two limits of that unit, both worth stating before quoting a percentage:
  * It is CODE LINES, not statements and not branches. A multi-line statement counts once, at the
    line the compiler attributes it to, and a line carrying several statements also counts once.
  * It is the DEBUG denominator, on purpose (see the /O2 trap above). The shipped build has a
    different one, and the figure is not a claim about shipped code.
And it does NOT double-count a header shared by many TUs: OpenCppCoverage merges per file, verified
on the campaign report -- 655 distinct files, zero appearing in more than one <class>, +0 inflation.

USAGE
    python tools/coverage.py --suite tacttest          # one offline oracle suite
    python tools/coverage.py --suite tacttest --suite simtest
    python tools/coverage.py --cmd "<exe> <args>"      # anything else
Reports a per-file table plus a cobertura XML under tmp/cov/, and an HTML report with --html.

RIG SCENARIOS ARE SUPPORTED (2026-09-04). This block used to say they were not, on the theory that
harness.cpp's TerminateProcess self-stop would cut off the collector's final write. Measured: it does
not -- TerminateProcess still raises an EXIT_PROCESS debug event, so the report is written (1.87 MB
in 13 s on poz-stand-exit). Use `--journal <recorded session>`; add `--debug-dll` when the number has
to be trustworthy on its own.

    python tools/coverage.py --journal tools/uiscripts/journals/poz3-teleport.journal --debug-dll
    python tools/coverage.py --baseline          # every registered scenario -> tools/data/*.json
    python tools/coverage.py --baseline --check  # DROPS-ONLY gate; a green run re-records
    python tools/coverage.py --baseline --check --dump-measured tmp/m.json   # keep the measurement
    python tools/coverage.py --baseline --check --from-measured tmp/m.json   # re-compare, no run

A RIG RUN'S CONFIG COMES FROM THE JOURNAL, not from the CLI defaults, and that is load-bearing:
`_rig_cmd` used to hardcode `tact_system = 0`, so a journal recorded on a non-default mission was
measured against the DEFAULT one and the report said the scenario never entered bodies it drives
constantly (POZ3: `tact_unit_teleport.cpp 0/97 0%` on a session carrying 118 teleport orders).
Honouring the header moved that run 53% -> 74%.
"""

import argparse
import collections
import glob
import json
import os
import re
import time
import subprocess
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _dllsrc  # noqa: E402
import machine_config as machine  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COV_DIR = os.path.join(REPO, "tmp", "cov")
DEFAULT_OCC = r"C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe"

# WHICH EXE ANSWERS TO A SUITE (fork F5I S4). There are two offline test executables since the
# libmh_test split -- net_selftest.exe (hosted arm) and libmh_selftest.exe (standalone arm) -- and
# which one carries a suite is tools/data/selftest_roster.json's business, not this file's. Routing
# matters here rather than merely being tidy: this tool's own default suite is `tacttest`, which
# moved, and building the wrong project would produce an exe that exits 2 on the suite name and a
# coverage report of nothing. Read, never re-listed.
ROSTER_PATH = os.path.join(REPO, "tools", "data", "selftest_roster.json")
_ROSTER = json.load(open(ROSTER_PATH, encoding="utf-8"))
SUITE_EXE = {r["suite"]: r["exe"] for r in _ROSTER["suites"]}
EXE_BUILD = _ROSTER["exes"]
# THE FLOOR: every exe the suite rows name must have a build recipe here. A roster that grows a
# third exe reds this import instead of silently routing its suites to one of the two that exist.
_unknown = sorted({r["exe"] for r in _ROSTER["suites"]} - set(EXE_BUILD))
if _unknown:
    raise SystemExit(
        "coverage.py: selftest_roster.json routes suite(s) to exe(s) with no `exes` entry: %s"
        % ", ".join(_unknown)
    )


def suite_build(suite):
    """(project path, exe basename) for a suite, from the roster. Unknown suite -> a named error,
    not a default: a typo used to build mh_nettest and measure a run that exited 2."""
    exe = SUITE_EXE.get(suite)
    if exe is None:
        raise SystemExit(
            "coverage.py: %r is not a roster suite. Known: %s"
            % (suite, ", ".join(sorted(SUITE_EXE)))
        )
    cfg = EXE_BUILD[exe]
    return os.path.join(REPO, *cfg["vcxproj"].split("/")), cfg["staged"]


def find_occ():
    p = getattr(machine, "OPENCPPCOVERAGE", None) or DEFAULT_OCC
    if os.path.isfile(p):
        return p
    from shutil import which

    return which("OpenCppCoverage.exe")


def _msbuild():
    msbuild = os.path.join(machine.VS_INSTALL_ROOT, "MSBuild", "Current", "Bin", "MSBuild.exe")
    return msbuild if os.path.isfile(msbuild) else None


def build_debug_dll():
    """Build an unoptimised mh.dll and return its path.

    THROUGH THE SOLUTION, NOT THE .vcxproj. Building src/mh_dll/mh/mh.vcxproj directly leaves
    $(SolutionDir) undefined, so every `#include "include/..."` that resolves through
    $(SolutionDir)mh_common fails -- which reads as a broken Debug configuration and is not one.
    That misreading cost a tracked item before the invocation was checked.
    """
    msbuild = _msbuild()
    if not msbuild:
        print("FAIL: MSBuild not found via machine_config.VS_INSTALL_ROOT")
        return None
    sln = os.path.join(REPO, "src", "mh_dll", "mh.sln")
    r = subprocess.run(
        [
            msbuild,
            sln,
            "/t:mh",
            "/p:Configuration=Debug",
            "/p:Platform=x86",
            "/m",
            "/nodeReuse:false",
            "/v:minimal",
            "/nologo",
        ],
        capture_output=True,
        text=True,
    )
    dll = os.path.join(REPO, "src", "mh_dll", "Debug", "mh.dll")
    if r.returncode != 0 or not os.path.isfile(dll):
        print("FAIL: debug mh.dll build failed (exit %d)" % r.returncode)
        print(r.stdout[-2000:])
        return None
    return dll


def build_debug(suite="tacttest"):
    """Build the unoptimised selftest THAT CARRIES THIS SUITE, and return its exe.

    Not piped through a filter -- a pipe hides the build's exit code."""
    vs = machine.VS_INSTALL_ROOT
    msbuild = os.path.join(vs, "MSBuild", "Current", "Bin", "MSBuild.exe")
    if not os.path.isfile(msbuild):
        print("FAIL: MSBuild not at %s (machine_config.VS_INSTALL_ROOT)" % msbuild)
        return None
    proj, exe_name = suite_build(suite)
    r = subprocess.run(
        [
            msbuild,
            proj,
            "/t:Build",
            "/p:Configuration=Debug",
            "/p:Platform=Win32",
            "/m",
            "/nodeReuse:false",
            "/v:minimal",
            "/nologo",
        ],
        capture_output=True,
        text=True,
    )
    exe = os.path.join(REPO, "src", "mh_dll", "Debug", exe_name)
    if r.returncode != 0 or not os.path.isfile(exe):
        print("FAIL: debug build of %s failed (exit %d)" % (os.path.basename(proj), r.returncode))
        print(r.stdout[-2000:])
        return None
    return exe


def _rig_cmd(args):
    """Provision a tactical lane for `args.journal` and return the game command line."""
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import test_ui

    jpath = args.journal if os.path.isabs(args.journal) else os.path.join(REPO, args.journal)
    if not os.path.isfile(jpath):
        raise SystemExit("FAIL: no such journal: %s" % jpath)
    ev = test_ui.tact_journal_read(jpath)
    frames = args.frames or (max(f for f, _k, _p in ev) + 200)

    # THE JOURNAL PICKS THE MISSION, exactly as it does for --tact-equiv. This block used to
    # hardcode tact_system=0 ("whatever the save says"), which is the same defect the gate had:
    # a journal recorded on a non-default mission was measured against the DEFAULT one, and the
    # coverage report then said the scenario never entered bodies it drives constantly. Measured on
    # the POZ3 journal -- `tact_unit_teleport.cpp 0/97 0%` on a session carrying 118 teleport
    # orders, because the run was not POZ3 at all.
    meta = test_ui.tact_journal_meta(jpath)

    def _int(key, dflt):
        raw = str(meta.get(key, "")).strip()
        return int(raw) if raw.isdigit() else dflt

    squad, owner, system = _int("squad", 8), _int("owner", 1), _int("system", 0)
    save = meta.get("save") or "11"

    class LaneArgs:
        visible = False
        headless = True
        no_desktop = True
        desktop = None
        force_headless = False
        stock_exe = False
        patched_exe = False
        tact_squad = squad
        tact_hp = 100
        tact_owner = owner
        tact_system = system
        tact_save = save

    lane = test_ui.tact_provision_lane(LaneArgs, visible=False)
    if lane is None:
        raise SystemExit("FAIL: could not provision a lane")
    test_ui.tact_write_config(
        lane,
        frames,
        0,
        -1,
        squad,
        100,
        owner,
        None,
        system,
        journal=os.path.abspath(jpath),
        journal_verify=1,
    )
    print(
        "  session   system=%s save=%s squad=%s owner=%s (from the journal header)"
        % (system or "from save", save, squad, owner)
    )
    for frag in getattr(args, "extra_ini", None) or []:
        test_ui.tact_merge_ini(lane, [frag])
        print("  extra-ini %s" % frag)
    lane = lane.replace("/", os.sep)
    if getattr(args, "debug_dll", False):
        dll = build_debug_dll()
        if not dll:
            raise SystemExit(1)
        import shutil

        shutil.copy2(dll, os.path.join(lane, "mh.dll"))
        pdb = dll[:-4] + ".pdb"
        if os.path.isfile(pdb):
            shutil.copy2(pdb, os.path.join(lane, "mh.pdb"))
        print("  dll       UNOPTIMISED build deployed into the lane (faithful attribution)")
    # CWD MUST BE THE LANE. The game resolves its data relative to the working directory, and a
    # run started elsewhere exits before the collector sees anything -- reported as "no report
    # written", which reads like the TerminateProcess hazard and is not it.
    return ([os.path.join(lane, "mh.focus.exe"), "--tactical", "11", "--skip-intro"], lane)


def summarise(xml_path, focus=()):
    r = ET.parse(xml_path).getroot()
    rows = {}
    for cls in r.iter("class"):
        name = os.path.basename(cls.get("filename", ""))
        lines = cls.findall(".//line")
        if not lines:
            continue
        hit = sum(1 for ln in lines if int(ln.get("hits", "0")) > 0)
        h, t = rows.get(name, (0, 0))
        rows[name] = (h + hit, t + len(lines))
    tot_h = sum(h for h, _ in rows.values())
    tot_t = sum(t for _, t in rows.values())
    print(
        "  overall   %d/%d lines = %.0f%%  across %d file(s)"
        % (tot_h, tot_t, (100.0 * tot_h / tot_t) if tot_t else 0.0, len(rows))
    )
    dead = sorted(n for n, (h, _t) in rows.items() if h == 0)
    if dead:
        print("  NEVER ENTERED by this run (%d file(s)):" % len(dead))
        for n in dead[:20]:
            print("     %s" % n)
        if len(dead) > 20:
            print("     ... and %d more" % (len(dead) - 20))
    for n in focus:
        for name, (h, t) in sorted(rows.items()):
            if n in name:
                print("  focus     %-38s %3d/%-4d %3.0f%%" % (name, h, t, 100.0 * h / t))
    return rows


def show_lines(xml_path, patterns, src_root):
    """Print hit/missed source lines for the matching files.

    A percentage answers "how much", never "which" -- and for these bodies the difference decides
    the question. tact_unit_stand_tick.cpp read 15% covered on a rig scenario, and all four hit
    lines were the retired differential oracle's registration -- code that ran once at arm time. The
    translated body itself was untouched. Reading the 15% as "partially exercised" would have been
    exactly backwards. The oracle went at F2D; the lesson (a percentage answers "how much", never
    "which") is why this prints lines.
    """
    r = ET.parse(xml_path).getroot()
    for cls in r.iter("class"):
        fn = cls.get("filename", "")
        if not any(p in os.path.basename(fn) for p in patterns):
            continue
        path = fn if os.path.isfile(fn) else os.path.join(src_root, os.path.basename(fn))
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                src = f.read().splitlines()
        except OSError:
            src = []
        marks = sorted(
            (int(ln.get("number")), int(ln.get("hits", "0"))) for ln in cls.findall(".//line")
        )
        hit = [m for m in marks if m[1] > 0]
        print(
            "  ---- %s: %d hit, %d missed" % (os.path.basename(fn), len(hit), len(marks) - len(hit))
        )
        for n, h in marks:
            text = src[n - 1].strip()[:76] if 0 < n <= len(src) else ""
            print(
                "     %s %4d %-8s %s" % ("HIT " if h else "miss", n, ("x%d" % h) if h else "", text)
            )


BASELINE_PATH = os.path.join(REPO, "tools", "data", "tact_coverage.json")


def baseline_path(domain):
    return os.path.join(REPO, "tools", "data", "%s_coverage.json" % domain)


# ---- PER-ROW attribution (OBS-ROWS) --------------------------------------------------------------
#
# A per-FILE figure cannot answer "did row X run" for this tree: 54% of sim's rows share a translation
# unit with another row (measured 2026-09-05 -- 484 rows over 303 files, 81 files holding two or more,
# the worst holding 29). So a green file speaks for one row only when it holds one row.
#
# The fix does not need new instrumentation, because both halves already exist: the Cobertura report
# carries per-LINE hits, and gen_liveness_census.py now emits each row's C++ leaf name (`ours`) beside
# its TU. What is left is to turn (tu, ours) into a set of LINES, which is what this block does.
#
# WHY ALL FUNCTIONS OF THAT NAME, not just the body. A translated row compiles to up to four entry
# points -- detail::X (the body), X() (the public wrapper a rebind targets), promoted_arm::X and,
# for a handful of rows, rebind_arm::X -- and WHICH of them is entered is exactly the fact under
# test. Taking the union
# answers the question actually being asked ("did this row execute at all") and stays correct whichever
# route the run used; keying on one arm would reproduce the very hole this replaces, where the counter
# sat on the entry-seam arm and a rebound call was invisible.


def _fn_spans(text, name):
    """Line spans of every function DEFINITION called exactly `name` in `text`.

    Comments are stripped line-preservingly first (tools/_cstrip -- the shared stripper, written after
    a strip-order bug in six tools made 21,199 characters of this same tree invisible). A definition is
    `name (...)` followed by `{`; a declaration ends in `;` and a call does not open a brace, so this
    one test separates all three without parsing C++.
    """
    spans = []
    for m in re.finditer(r"\b%s\s*\(" % re.escape(name), text):
        i, depth = m.end() - 1, 0
        while i < len(text):  # walk to the ) that closes the parameter list
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        j = i + 1
        while j < len(text) and text[j] not in "{;":
            j += 1
        if j >= len(text) or text[j] != "{":
            continue  # a declaration or a call, not a definition
        start = text.count("\n", 0, m.start()) + 1
        depth, k = 0, j
        while k < len(text):
            if text[k] == "{":
                depth += 1
            elif text[k] == "}":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        spans.append((start, text.count("\n", 0, k) + 1))
    return spans


def _name_index(src_roots):
    """{function name: [tu relative paths that DEFINE it]} over the module trees.

    TWO ROOTS SINCE FORK F5O (mh/ and libmh/), keyed module-relative against whichever root
    holds the file -- so a row's `tu` (`sim/sim_rng_next.cpp`, straight out of the liveness
    census) resolves exactly as it did when one directory held everything.

    Built once per row_spans call and only when some row needs it. Deliberately indexes DEFINITIONS
    (via _fn_spans, which separates a definition from a declaration and a call) rather than mentions,
    so a header declaration or a call site cannot claim a name.
    """
    index = collections.defaultdict(list)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from _cstrip import strip_comments

    for tree, root, _dirs, files in _dllsrc.walk(src_roots):
        if any(part in root for part in ("attic", "Debug", "Release", "selftest")):
            continue
        for fn in files:
            if not fn.endswith(".cpp"):
                continue
            path = os.path.join(root, fn)
            try:
                with open(path, encoding="utf-8", errors="replace") as fh:
                    text = strip_comments(fh.read(), keep_lines=True)
            except OSError:
                continue
            rel = _dllsrc.rel_or_raise(path)
            # CANDIDATE names first, then _fn_spans confirms each is a real definition. The cheap
            # regex over-matches on purpose (calls, declarations, control keywords); _fn_spans is
            # the arbiter, and running it per candidate rather than per file keeps ONE definition
            # of "is a definition" in the tool.
            for name in set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", text)):
                if rel not in index[name] and _fn_spans(text, name):
                    index[name].append(rel)
    return index


def _resolve_tu(src_roots, rel):
    """A module-relative TU path -> the tree that actually holds it (F5O: mh/ or libmh/)."""
    for tree in src_roots:
        p = os.path.join(tree, rel.replace("/", os.sep))
        if os.path.isfile(p):
            return p
    return os.path.join(src_roots[0], rel.replace("/", os.sep))


def row_spans(domain):
    """{row_name: {"tu":…, "ours":…, "spans":[(lo,hi)…]}} plus the rows that cannot be attributed."""
    path = os.path.join(REPO, "tools", "data", "%s_liveness.json" % domain)
    if not os.path.isfile(path):
        # FROZEN INPUT SINCE FORK F2E. gen_liveness_census.py used to regenerate this; its attribution
        # WAS the shadow scan, which F2D deleted, and the replacement bridge measured worse. The
        # committed census is the last measurement and there is no command to re-take it, so a missing
        # file is a checkout problem, not a stale one.
        return {}, [
            ("(no census)", "tools/data/%s_liveness.json is missing from the checkout" % domain)
        ]
    rows = json.load(open(path, encoding="utf-8"))["rows"]
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from _cstrip import strip_comments

    src_roots = _dllsrc.ROOTS
    cache, out, unattributed = {}, {}, []
    tree = None
    for r in rows:
        tu, ours = r.get("tu"), r.get("ours")
        if ours and not tu:
            # NO TU, BUT A NAME: find the TU by searching the tree for a UNIQUE definition of it. The
            # census resolves `tu` from where it found the row's binder or install site, and for an
            # entry_seam row that site is the domain's promote table rather than the body -- so a whole
            # domain can arrive with `tu: null` and every row correct. Measured 2026-09-05: all 32
            # sim_resid rows, and 46 of sim's 530.
            #
            # UNIQUE IS THE WHOLE RULE. Two TUs defining the same short name means the address this row
            # actually runs is not derivable from the name, and crediting either one would be a guess
            # that reads as a measurement. Those stay unattributed, exactly as the census DROPS a leaf
            # two rows both claim.
            if tree is None:
                tree = _name_index(src_roots)
            hits = tree.get(ours, ())
            # SEVERAL TUs DEFINING THE NAME IS NORMAL, AND THE UNION IS THE RIGHT ANSWER -- this
            # started out reporting them as ambiguous and unattributed, which threw away all 32
            # sim_resid rows. The two definitions are the BODY (sim/resid/sim_<row>.cpp) and the
            # PROMOTION ADAPTER of the same name in resid_promote.cpp; they are two definitions of one
            # row, not two rows, so a hit in either means this row executed.
            #
            # WHAT MAKES THAT SAFE, rather than a convenient assumption: the census guarantees `ours` is
            # unique per row within a domain -- _row_short_names DROPS a leaf that two rows both claim
            # rather than assigning it to either. So a name reaching here belongs to exactly one row,
            # and every TU defining it is a definition of that row. Asserted below rather than trusted.
            tu = list(hits) if hits else None
        if not tu or not ours:
            unattributed.append((r["name"], "no TU" if not tu else "no C++ leaf name"))
            continue
        tus = tu if isinstance(tu, list) else [tu]
        spans, basenames, unreadable = [], [], []
        for one in tus:
            if one not in cache:
                p = _resolve_tu(src_roots, one)
                try:
                    with open(p, encoding="utf-8", errors="replace") as fh:
                        cache[one] = strip_comments(fh.read(), keep_lines=True)
                except OSError:
                    cache[one] = None
            text = cache[one]
            if text is None:
                unreadable.append(one)
                continue
            got = _fn_spans(text, ours)
            if got:
                spans.append((os.path.basename(one), got))
                basenames.append(os.path.basename(one))
        if not spans and unreadable:
            unattributed.append((r["name"], "TU not readable: %s" % ", ".join(unreadable)))
            continue
        if not spans:
            # NOT a guess-and-move-on. An unfound definition means the leaf lives in a header, or the
            # name was resolved to something this TU does not define -- either way the honest output is
            # "unattributed", because a row silently credited to the wrong lines is worse than a gap.
            unattributed.append((r["name"], "no definition of %s() in %s" % (ours, tus)))
            continue
        out[r["name"]] = {
            "tu": basenames[0],
            "tus": basenames,
            "ours": ours,
            "route": r.get("route"),
            # [(tu basename, [(lo,hi)...])...] -- per TU, because a hit is looked up by BASENAME in the
            # Cobertura report and two TUs have two different basenames.
            "spans": spans,
        }

    # THE UNIQUENESS THE UNION RELIES ON, asserted rather than assumed (see the note above). If two rows
    # of this domain ever resolved to the same leaf, a hit in one would be credited to both.
    seen = {}
    for name, info in list(out.items()):
        prev = seen.setdefault(info["ours"], name)
        if prev != name:
            for n in (prev, name):
                out.pop(n, None)
                unattributed.append((n, "leaf %s() claimed by 2 rows -- dropped" % info["ours"]))
    return out, unattributed


def _ranges(nums):
    """[1,2,3,7,9,10] -> "1-3, 7, 9-10". Line lists are read by people; a 90-element comma list is not."""
    out, start, prev = [], nums[0], nums[0]
    for n in nums[1:]:
        if n == prev + 1:
            prev = n
            continue
        out.append(str(start) if start == prev else "%d-%d" % (start, prev))
        start = prev = n
    out.append(str(start) if start == prev else "%d-%d" % (start, prev))
    return ", ".join(out)


def hit_lines(xml_path):
    """{basename: set(line numbers with hits > 0)} from a Cobertura report.

    Hits are BINARY here and that is a property of OpenCppCoverage, not of this reader: measured over
    tmp/cov/dbg.xml the only values present are 0 and 1. So this channel answers ">=1 execution" and
    can never answer "how many" -- sufficient for the anti-vacuity question (did the body run at all),
    insufficient for a hot-versus-cold reading.
    """
    hits = {}
    for cls in ET.parse(xml_path).getroot().iter("class"):
        name = os.path.basename(cls.get("filename", ""))
        s = hits.setdefault(name, set())
        for ln in cls.findall(".//line"):
            if int(ln.get("hits", "0")) > 0:
                s.add(int(ln.get("number")))
    return hits


def attribute(xml_path, domain):
    """(ran, cold, unattributed) row-name lists for one report."""
    spans, unattributed = row_spans(domain)
    hits = hit_lines(xml_path)
    ran, cold = [], []
    for name, info in sorted(spans.items()):
        hot = False
        for base, ranges in info["spans"]:
            got = hits.get(base, ())
            if any(lo <= n <= hi for lo, hi in ranges for n in got):
                hot = True
                break
        (ran if hot else cold).append(name)
    return ran, cold, unattributed


def _uirec_cmd(args):
    """Provision a GAME-START lane for `args.journal` and return the game command line.

    The sibling of `_rig_cmd`, and it has to be a sibling rather than a branch inside it: a tactical
    journal names a mission (squad/owner/system/save) and drops the player straight into mode 6,
    while a UI-REC journal starts at the MAIN MENU and the recording itself walks into the campaign.
    Different lane, different config block, nothing shared but the shape of the return.

    PROMOTED, i.e. the SHIP arm, and that is the whole point of measuring this fixture: the question
    coverage answers is "which of OUR bodies did this session actually execute", and the
    all-original arm executes none of them by construction. `ui_write_config`'s defaults ARE the ship
    config, so this passes no rollback ini -- run_ui_abc is the arm that does.

    The budget is the journal's own (`ui_budget`), not a guess: too small a budget ends the run while
    input is still pending, and the coverage figure then describes a prefix of the session with no
    line anywhere saying so."""
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import test_ui

    jpath = args.journal if os.path.isabs(args.journal) else os.path.join(REPO, args.journal)
    if not os.path.isfile(jpath):
        raise SystemExit("FAIL: no such journal: %s" % jpath)

    class LaneArgs:
        visible = False
        headless = True
        no_desktop = True
        desktop = None
        force_headless = False
        stock_exe = False
        patched_exe = False
        fps_limit = None
        ui_steps = 0

    lane = test_ui.ui_provision_lane(LaneArgs, visible=False, slot=0)
    if lane is None:
        raise SystemExit("FAIL: could not provision a game-start lane")
    test_ui.ui_write_config(
        lane,
        journal=jpath,
        stop_step=test_ui.ui_budget(LaneArgs, jpath),
        visible=False,
    )
    # THE LANE GETS THE RELEASE DLL UNLESS TOLD OTHERWISE -- make_lane.deploy_dll's source is
    # src/mh_dll/Release/mh.dll -- so this MUST overwrite it, exactly as the tactical path does.
    # Measured the day this was written: without it the campaign report was a strict SUBSET of the
    # tactical one (655 files against 700, and 649 of the 655 shared files with FEWER instrumented
    # lines, never more) because /O2+LTCG had inlined the small bodies out of existence. That is the
    # precise failure this whole tool exists to refuse, arriving through a lane provisioner that had
    # no idea it was serving a coverage run.
    if getattr(args, "debug_dll", False):
        dll = build_debug_dll()
        if not dll:
            raise SystemExit(1)
        import shutil

        shutil.copy2(dll, os.path.join(lane, "mh.dll"))
        pdb = dll[:-4] + ".pdb"
        if os.path.isfile(pdb):
            shutil.copy2(pdb, os.path.join(lane, "mh.pdb"))
        print("  dll       UNOPTIMISED build deployed into the lane (faithful attribution)")
    return [os.path.join(lane, "mh.focus.exe"), "--skip-intro"], lane


def measure_one(args, occ):
    """Run ONE rig journal under the collector; return its cobertura path or None.

    Split out of main() so --baseline can drive the same path per scenario rather than reproducing
    it -- a second copy of the collector invocation is a second place for the --sources absoluteness
    trap to come back."""
    # WHICH KIND OF JOURNAL IS THIS? A UI-REC (game-start) journal carries no tactical header and
    # would be measured against a DEFAULT mission if handed to _rig_cmd -- the exact failure this
    # file's header records for POZ3, with the roles swapped. Dispatch on the domain, not on a flag
    # a caller might forget to set.
    rig_cmd, rig_cwd = (_uirec_cmd if getattr(args, "uirec", False) else _rig_cmd)(args)
    name = "rig_%s" % os.path.splitext(os.path.basename(args.journal))[0]
    xml_path = os.path.join(COV_DIR, "%s.xml" % name)
    os.makedirs(COV_DIR, exist_ok=True)
    occ_args = [
        occ,
        "--quiet",
        "--sources",
        os.path.abspath(args.sources),
        "--export_type",
        "cobertura:%s" % xml_path,
        "--",
    ] + rig_cmd
    r = subprocess.run(occ_args, capture_output=True, text=True, cwd=rig_cwd)
    if not os.path.isfile(xml_path):
        for ln in (r.stderr or "").splitlines()[-6:]:
            print("      %s" % ln[:110])
        return None
    return xml_path


def report_rows(xml_path, domain):
    """Print the per-row execution table for one report, split by liveness ROUTE.

    THE SPLIT BY ROUTE IS THE POINT, not decoration. Before this, the only per-row call evidence in
    the tree was the `served call #N` counter, which sits on the entry-seam arm -- so `rebind_member`,
    the class carrying 396 of sim's 530 rows, had no execution evidence of any kind. Reporting the
    route breakdown is what makes it visible that the rebound rows are being measured at all, and it
    is the first thing to check if the total looks plausible: a run whose rebind_member count is zero
    has measured the entry seams and nothing else.
    """
    spans, _un = row_spans(domain)
    ran, cold, unattributed = attribute(xml_path, domain)
    total = len(ran) + len(cold) + len(unattributed)
    print(
        "  rows      %d of %d verified rows RAN  (%d cold, %d unattributed)"
        % (len(ran), total, len(cold), len(unattributed))
    )
    by_route = collections.defaultdict(lambda: [0, 0])
    for name in ran:
        by_route[spans[name]["route"]][0] += 1
    for name in cold:
        by_route[spans[name]["route"]][1] += 1
    for route, (r, c) in sorted(by_route.items(), key=lambda kv: -(kv[1][0] + kv[1][1])):
        print("     %-28s %4d ran / %4d cold" % (route, r, c))
    if unattributed:
        print(
            "     UNATTRIBUTED %d row(s) -- no verdict either way; e.g. %s"
            % (len(unattributed), ", ".join(n for n, _w in unattributed[:3]))
        )
    return ran, cold, unattributed


def _files_of(entry):
    """The per-file half of a baseline entry, tolerating the pre-2026-09-05 flat shape.

    The committed tact record maps scenario -> {file: [hit,total]} directly. The sim record adds a row
    half, so entries became {"files":…, "rows_ran":…}. Reading both keeps the older committed file
    valid instead of forcing a re-record, which would have meant a rig sweep to land a format change.
    """
    if isinstance(entry, dict) and "files" in entry and isinstance(entry.get("files"), dict):
        return entry["files"]
    return entry if isinstance(entry, dict) else {}


def compare_rows(new, old, census=None, unattributed=None):
    """Drops-only over {scenario: [row names that RAN]}.

    THE STRONGER HALF OF THE GATE, and the reason the row map exists. A file's covered-line count
    moves whenever anyone edits the file, so a drop there is often noise. A row that USED to execute
    and now does not is never noise: either the scenario lost reach or the row lost its route, and the
    second is exactly the silent regression the liveness census was built to catch -- a row counted
    LIVE that nothing actually calls. Direction matters the same way it does for files: going cold
    FAILS, going warm never does.

    WHAT THE 2026-09-11 REDESIGN ADDED: a baseline row can stop being MEASURABLE without any coverage
    being lost. Regenerating `<domain>_liveness.json` renames, merges or drops rows, and a row the
    census no longer contains cannot be cold -- there is nothing left to execute. Likewise a row the
    run leaves UNATTRIBUTED carries, by the attributor's own definition, no verdict either way, so
    failing on it fails on the instrument rather than on coverage. Both are reported and absorbed;
    only a row STILL IN THE CENSUS, still attributable, that used to run and now does not is red.
    `census`/`unattributed` are None for the pure arms, which then get the old strict behaviour.
    """
    census = None if census is None else set(census)
    unatt = set(unattributed or ())
    out = {"cold": [], "census_dropped": [], "no_verdict": [], "warm": [], "added": []}
    for scen, ran in sorted(new.items()):
        prev = old.get(scen)
        if prev is None:
            out["added"].append("%s (whole scenario)" % scen)
            continue
        was, now = set(prev), set(ran)
        for n in sorted(was - now):
            if census is not None and n not in census:
                out["census_dropped"].append((scen, n))
            elif n in unatt:
                out["no_verdict"].append((scen, n))
            else:
                out["cold"].append((scen, n))
        out["warm"] += [(scen, n) for n in sorted(now - was)]
    return out


def core_and_flaky_lines(per_run_lines):
    """Split N runs' {file: [lines]} into the reproducible CORE and the FLAKY remainder.

    Pure, and extracted so it is checkable without a rig -- the same lesson the soak premise taught on
    2026-09-05 (a rule that asserted the wrong thing all day with nobody able to check it offline).

    Core = intersection over every run, which is what the gate compares; flaky = union minus core,
    which it never compares. A file absent from a run contributes the empty set, so a file only SOME
    runs saw has an empty core and its whole line set flaky -- deliberately: an unmeasured file is not
    evidence of coverage, and pinning run 1's view of it would gate on a coin flip.
    """
    core, flaky = {}, {}
    names = set()
    for r in per_run_lines:
        names |= set(r)
    for fname in sorted(names):
        sets = [set(r.get(fname, ())) for r in per_run_lines]
        s = set.intersection(*sets) if sets else set()
        u = set.union(*sets) if sets else set()
        core[fname] = sorted(s)
        if u - s:
            flaky[fname] = sorted(u - s)
    return core, flaky


def registered_journals():
    """The journals the tactical gate runs, DERIVED from test_ui.py's TACT_SCENARIOS."""
    src = open(os.path.join(REPO, "tools", "test_ui.py"), encoding="utf-8", errors="replace").read()
    m = re.search(r"TACT_SCENARIOS\s*=\s*\[(.*?)\n\]", src, re.S)
    return re.findall(r'"journal":\s*"([^"]+)"', m.group(1)) if m else []


BANNER = """\
------------------------------------------------------------------------------
DROPS-ONLY GATE (redesigned 2026-09-11, user's ruling -- tracker TACT-COV-YIELD)
  RED    one of THIS DOMAIN'S OWN files lost more covered lines than its own edit
         can explain; a row still in the census went COLD; one of its own files
         VANISHED from the report with real code still in the tree
  INFO   gains, new files, new scenarios, reshaped files, rows the census dropped,
         and every FOREIGN file's movement in either direction
WHAT A COUNT GATE CANNOT SEE -- do not read a PASS as more than it is:
  * an EQUAL-COUNT SWAP. N lines of one body going cold while N lines of another
    light up is arithmetically invisible here. The row half is the only cover.
  * a drop INSIDE an edited file, up to the number of instrumented lines that
    edit removed. Absorbed as `reshaped`, by construction.
  * FOREIGN movement. These scenarios drive the whole DLL, so most of the record
    is other domains' files, and they are gated where their own scenarios drive
    them. MEASURED at HEAD, not assumed: 632 of tact's 657 foreign files are in
    another domain's committed baseline. The other 25 are NEWER than that
    baseline, so nothing gates them yet -- the old rule did not either (they
    were `added`), and re-recording the other domain is what closes it.
------------------------------------------------------------------------------"""


def write_baseline(path, domain, record):
    """The committed record, written from exactly one place -- --baseline and the green absorb."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(
            {
                "_generated_by": "tools/coverage.py --baseline --domain %s" % domain,
                "_measures": "per-file covered/total LINES, and per-ROW did-it-execute, for each "
                "registered scenario -- from an UNOPTIMISED build (an optimised one reports inlined "
                "bodies as 0%). Row attribution comes from <domain>_liveness.json's tu+ours fields.",
                "_compare": "DROPS-ONLY (2026-09-11). --check FAILS on: one of THIS DOMAIN'S OWN "
                "files (the TUs its liveness census names) losing more covered lines than the lines "
                "its own edit removed; a row still in the census going "
                "COLD; a baseline file vanishing from the report while its source still exists. "
                "Foreign files -- the rest of the DLL these scenarios also drive -- are counted and "
                "printed but gated by the domain whose scenarios drive them. Gains, "
                "new files, reshaped files and census-dropped rows are absorbed with an info line, and "
                "a GREEN run re-records this file so it tracks the tree instead of ageing into a "
                "record no measurement can match. The ROW half is the stronger one -- a row going cold "
                "means the scenario lost reach or the row lost its route, and the second is silent.",
                "scenarios": record,
            },
            f,
            indent=1,
            sort_keys=True,
        )
        f.write("\n")


def classify_file_delta(ohit, otot, hit, tot):
    """(verdict, allowance, note) for one file's `[hit, total]` pair against its baseline pair.

    PURE, and the core of the 2026-09-11 drops-only redesign (TACT-COV-YIELD, the user's ruling).
    The old rule was `hit < ohit -> red`, full stop, and it went red three gate readings running on
    changes that lost no coverage at all -- because the covered-line COUNT is only comparable while
    the file's INSTRUMENTED-LINE POPULATION is the same. Edit the file and `tot` moves, at which
    point `ohit` is a number about a file that no longer exists. The extreme signature this gate
    actually produced is `covered lines 40 -> 34 (of 34)`: a baseline claiming 40 covered lines in a
    file that now has only 34 lines to cover, which is not a regression, it is a stale entry.

    THE RULE.
      * `tot == otot` -- the file's shape is unchanged, so the counts ARE comparable and the arm is
        SHARP: losing even one line is red.
      * `tot != otot` -- the file was edited. At most `otot - tot` covered lines can have vanished
        because the LINES vanished, so that is the allowance; a drop inside it is `reshaped` and
        absorbed, a drop beyond it is real reach lost and is still red, NAMED with both numbers.
      * a file that GREW gets allowance 0: adding lines cannot explain losing coverage.
    A gain is never red, whatever happened to `tot`.

    WHAT THIS DELIBERATELY CANNOT SEE -- and the banner says so at every run: inside an edited file,
    a drop up to the allowance is invisible, and at ANY time an equal-count swap (N lines of one body
    go cold while N lines of another light up) reads as no change at all. The row half is what covers
    that axis; a per-file scalar never can.
    """
    note = ""
    if ohit > tot:
        # The baseline claims more covered lines than the file now HAS. Impossible against the
        # current tree, so the entry is provably stale -- worth saying out loud even when absorbed,
        # because it is the signature that a baseline has fallen behind rather than a code change.
        note = "baseline's %d covered exceeds the file's %d instrumented lines -- stale entry" % (
            ohit,
            tot,
        )
    if hit > ohit:
        return "gain", 0, note
    if hit == ohit:
        return "same", 0, note
    allowance = max(0, otot - tot)
    lost = ohit - hit
    if tot == otot:
        return "drop", 0, note or "file shape unchanged (%d instrumented lines)" % tot
    if lost <= allowance:
        return (
            "reshaped",
            allowance,
            note
            or "%d fewer covered, but %d instrumented lines were removed (%d -> %d)"
            % (lost, allowance, otot, tot),
        )
    return (
        "drop",
        allowance,
        "lost %d covered lines, only %d instrumented lines removed (%d -> %d)"
        % (lost, allowance, otot, tot),
    )


def dispositions_path(domain):
    return os.path.join(REPO, "tools", "data", "%s_coverage_dispositions.json" % domain)


def load_dispositions(domain):
    """{"files": {key: reason}, "rows": {key: reason}} -- why a VANISHED entry is not a regression.

    A file that disappears from the report entirely is the one removal shape that IS a regression
    signal: the file still exists and still compiles, but nothing in it is instrumented any more, so
    it dropped out of the build or out of `--sources`. That is coverage lost by construction, and it
    used to be absorbed silently. It is red now, and this file is how a deliberate one is recorded --
    a reason in writing, not a rule the tool relaxes.
    """
    p = dispositions_path(domain)
    if not os.path.isfile(p):
        return {"files": {}, "rows": {}}
    d = json.load(open(p, encoding="utf-8"))
    return {"files": d.get("files", {}) or {}, "rows": d.get("rows", {}) or {}}


def _has_instrumentable_code_not(name):
    """`empty_src` predicate for compare_baseline -- true when there is nothing left to instrument."""
    return not _has_instrumentable_code(name)


def domain_owned_files(domain):
    """The basenames of the TUs holding THIS domain's verified rows, from its own census.

    WHY THE FILE HALF IS SCOPED (2026-09-11, the measured half of the drops-only redesign). A tactical
    journal drives the whole DLL, so `tact_coverage.json` records ~700 files and only 64 of them hold a
    tact row. Every gate reading that went red in the week before this redesign was driven by the other
    636: `mh_calls.gen.cpp` lost 60 covered lines because LIB-REF-IN deliberately routed 85 hosted seams
    away from the `mh::call::` table, and `resid_promote.cpp` lost 8 for the same reason, in the same
    commit. Neither is a fact about tactical coverage, and failing the TACT gate on them is a category
    error -- which is precisely the diagnosis the user's ruling rests on.

    IT IS NOT A HOLE, and that was checked rather than assumed: every one of those files is ALSO in
    `sim_coverage.json`, at a higher covered count, because the sim soaks are the scenarios that
    actually exercise them. The domain that drives a file is the domain that gates it. Foreign movement
    is still counted and printed here -- loudly, and in both directions -- it just does not fail.

    DERIVED, NOT GUESSED. The set comes from the census's `tu` fields, not from a `tact_` name prefix:
    a prefix test is an instrument that reads a name instead of measuring (G171), and it would have
    quietly mis-scoped the day someone names a file without the prefix. Header siblings count as owned
    because a translated body may live in the .h.
    """
    p = os.path.join(REPO, "tools", "data", "%s_liveness.json" % domain)
    if not os.path.isfile(p):
        return None  # no census for this domain -- scope everything, i.e. the old behaviour
    rows = json.load(open(p, encoding="utf-8")).get("rows") or []
    owned = set()
    for r in rows:
        tu = r.get("tu")
        if not tu:
            continue
        base = os.path.basename(tu)
        owned.add(base)
        stem = os.path.splitext(base)[0]
        owned |= {stem + ext for ext in (".h", ".hpp", ".inl", ".cpp")}
    return owned or None


def _has_instrumentable_code(name, src_root=None):
    """Does a source file with this basename still contain anything a collector could instrument?

    THE THIRD ANSWER to a vanished baseline entry, and a measured one. `rebind_traps.gen.cpp` dropped
    out of every report at HEAD while its source sat right there in the tree -- because the generator
    now emits ZERO traps, so the file is a banner, two #includes and an empty namespace. That is the
    opposite of a regression: it means every rebind row found a body. A file that fell out of the BUILD
    still has its code, so stripping comments and preprocessor lines separates the two cases without
    needing a disposition for the benign one.
    """
    sys.path.insert(0, os.path.join(REPO, "tools"))
    from _cstrip import strip_comments

    root = src_root or os.path.join(REPO, "src", "mh_dll")
    for dirpath, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in ("Debug", "Release", ".vs")]
        if name not in files:
            continue
        with open(os.path.join(dirpath, name), encoding="utf-8", errors="replace") as fh:
            code = strip_comments(fh.read(), keep_lines=True)
        for ln in code.splitlines():
            s = ln.strip()
            if not s or s.startswith("#"):
                continue
            if (
                s.rstrip(";") in ("{", "}", "")
                or s.startswith("namespace ")
                or s.startswith("using ")
            ):
                continue
            return True
        return False
    return False


def _source_basenames(src_root=None):
    """Every source basename in the DLL tree -- so a vanished ENTRY can be told from a deleted FILE.

    A file the tree no longer contains cannot have lost coverage; it was deleted or renamed, and the
    measured record is simply newer than the baseline. Absorbed with an info line. A file still on
    disk that produced no instrumented lines is the real signal, and stays red.
    """
    root = src_root or os.path.join(REPO, "src", "mh_dll")
    names = set()
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            if f.endswith((".c", ".cpp", ".h", ".hpp", ".inl")):
                names.add(f)
    return names


def compare_baseline(new, old, dispositions=None, source_names=None, owned=None, empty_src=None):
    """Drops-only between two {scenario: {file: [hit, total]}} records.

    REGRESSION-ONLY, and that is the whole design. Coverage is not a value to pin: it moves whenever
    a source file is edited, so an equality check would go red on every commit and be disabled
    within a week. What is worth failing on is a file that USED to be entered and now is not, or one
    whose covered-line count fell BY MORE THAN ITS OWN EDIT EXPLAINS -- that is a scenario quietly
    losing reach, which is exactly the drift that hid G115 (the tick chain sat at 0% of body lines
    while the suite stayed green). A GAIN is reported and never failed on; so are new files and new
    scenarios. A VANISHED file is red unless the tree no longer has it or it carries a disposition.

    SCOPED, when `owned` is given: only the domain's own TUs can produce a red. Foreign drops go to
    their own bucket so they are still counted and printed -- see domain_owned_files() for why, and
    for the measurement that says this is a re-scoping rather than a hole.

    Returns a dict of buckets. It stopped being a 4-tuple in the 2026-09-11 redesign because the
    verdict now has five outcomes, and a positional API would have made the absorbed ones easy to
    read as failures at the call site -- which is the exact confusion the redesign exists to end.
    """
    disp = dispositions or {"files": {}, "rows": {}}
    known = None if source_names is None else set(source_names)
    own = None if owned is None else set(owned)
    empty = empty_src if empty_src is not None else (lambda _n: False)
    out = {
        "drops": [],
        "foreign_drops": [],
        "gains": [],
        "reshaped": [],
        "stale": [],
        "added": [],
        "vanished_red": [],
        "vanished_ok": [],
    }
    for scen, files in sorted(new.items()):
        prev = old.get(scen)
        if prev is None:
            out["added"].append("%s (whole scenario)" % scen)
            continue
        for name, (hit, tot) in sorted(files.items()):
            if name not in prev:
                out["added"].append("%s/%s" % (scen, name))
                continue
            ohit, otot = prev[name][0], prev[name][1]
            verdict, allowance, note = classify_file_delta(ohit, otot, hit, tot)
            row = (scen, name, ohit, otot, hit, tot, note)
            if verdict == "drop":
                out["drops" if (own is None or name in own) else "foreign_drops"].append(row)
            elif verdict == "gain":
                out["gains"].append(row)
            elif verdict == "reshaped":
                out["reshaped"].append(row)
            # STALE means PROVABLY stale, not merely moved: the baseline claims more covered lines
            # than the file now has to cover, so no measurement of this tree could ever reproduce the
            # entry. Keeping it to that test is what makes the count worth reading -- an earlier draft
            # put every reshaped row here too, and a bucket that equals its neighbour says nothing.
            if ohit > tot:
                out["stale"].append(row)
        for name in sorted(prev):
            if name in files:
                continue
            key = "%s/%s" % (scen, name)
            why = disp["files"].get(key) or disp["files"].get(name)
            if why:
                out["vanished_ok"].append((key, "dispositioned: %s" % why))
            elif known is not None and name not in known:
                out["vanished_ok"].append((key, "no such source file in the tree any more"))
            elif empty(name):
                out["vanished_ok"].append(
                    (key, "the source has no instrumentable code left -- nothing to cover")
                )
            elif own is not None and name not in own:
                out["vanished_ok"].append(
                    (key, "FOREIGN to this domain -- gated by the domain whose scenarios drive it")
                )
            else:
                out["vanished_red"].append(
                    (key, "still in the tree but produced NO instrumented lines")
                )
    return out


def selftest_compare():
    """The drift comparison's own arms, offline. A comparison whose FAILING case is never exercised
    is a check nobody has seen work -- and this one is regression-only by design, so the easy bug is
    a rule that quietly never fires."""
    bad = 0

    # THE SHIFT-TOLERANCE RULE'S OWN ARMS (2026-09-11). classify_file_delta is pure and is the whole
    # of the redesign's judgment, so it is asserted directly and in BOTH directions: every arm names
    # the verdict it must produce, and the table below contains at least one arm for each of the four
    # verdicts plus the two boundary cases the rule turns on (a drop exactly AT the allowance, and one
    # line past it). An arm set that only ever expected "drop" would pass against a rule that always
    # says drop -- the mutation pass at the end of this function is what proves it does not.
    dcases = [
        # (name,               ohit otot  hit  tot   want)
        ("shape same, equal", 10, 20, 10, 20, "same"),
        ("shape same, gain", 10, 20, 14, 20, "gain"),
        ("shape same, -1 line", 10, 20, 9, 20, "drop"),
        # The signature this redesign exists for: `covered lines 40 -> 34 (of 34)`. Ten instrumented
        # lines went away, six covered ones went with them -- absorbed, and flagged stale because the
        # baseline's 40 is impossible against a 34-line file.
        ("the 40-of-34 reshape", 40, 44, 34, 34, "reshaped"),
        ("drop exactly at allowance", 10, 20, 8, 18, "reshaped"),
        ("drop one past allowance", 10, 20, 7, 18, "drop"),
        # Growth cannot explain a loss, so the allowance is zero in that direction.
        ("file GREW and lost a line", 10, 20, 9, 26, "drop"),
        ("file grew and gained", 10, 20, 15, 26, "gain"),
    ]
    for name, ohit, otot, hit, tot, want in dcases:
        got, _allow, _note = classify_file_delta(ohit, otot, hit, tot)
        good = got == want
        print("  %-28s %s" % (name, "ok" if good else "FAIL got %s want %s" % (got, want)))
        bad += 0 if good else 1
    stale_flag = classify_file_delta(40, 44, 34, 34)[2]
    good = "stale" in stale_flag
    print("  %-28s %s" % ("impossible entry is named", "ok" if good else "FAIL %r" % stale_flag))
    bad += 0 if good else 1

    base = {"s1": {"a.cpp": [10, 20], "b.cpp": [5, 10]}}
    SRC = {"a.cpp", "b.cpp", "c.cpp"}
    cases = [
        ("identical is silent", base, {}, lambda r: not any(r[k] for k in r)),
        (
            "a DROP is a regression",
            {"s1": {"a.cpp": [7, 20], "b.cpp": [5, 10]}},
            {},
            lambda r: len(r["drops"]) == 1 and not r["gains"],
        ),
        (
            "a GAIN never fails",
            {"s1": {"a.cpp": [14, 20], "b.cpp": [5, 10]}},
            {},
            lambda r: r["gains"] and not r["drops"],
        ),
        (
            "a RESHAPE never fails",
            {"s1": {"a.cpp": [8, 18], "b.cpp": [5, 10]}},
            {},
            lambda r: r["reshaped"] and not r["drops"],
        ),
        (
            "a NEW file never fails",
            {"s1": {"a.cpp": [10, 20], "b.cpp": [5, 10], "c.cpp": [3, 4]}},
            {},
            lambda r: r["added"] and not r["drops"],
        ),
        # THE REMOVED-FILE RULE INVERTED (2026-09-11). It used to be absorbed unconditionally, which
        # made the one genuinely alarming removal -- a file still in the tree that stopped producing
        # instrumented lines, i.e. it fell out of the build -- indistinguishable from a deletion.
        (
            "a VANISHED file FAILS",
            {"s1": {"a.cpp": [10, 20]}},
            {},
            lambda r: r["vanished_red"] and not r["vanished_ok"],
        ),
        (
            "...unless dispositioned",
            {"s1": {"a.cpp": [10, 20]}},
            {"files": {"s1/b.cpp": "moved into the header, measured there"}, "rows": {}},
            lambda r: r["vanished_ok"] and not r["vanished_red"],
        ),
        (
            "a NEW scenario never fails",
            {"s1": base["s1"], "s2": {"z.cpp": [1, 2]}},
            {},
            lambda r: r["added"] and not r["drops"],
        ),
    ]
    for name, new, disp, ok in cases:
        res = compare_baseline(new, base, dispositions=disp or None, source_names=SRC)
        good = bool(ok(res))
        print("  %-28s %s" % (name, "ok" if good else "FAIL"))
        bad += 0 if good else 1
    # The deleted-source arm needs a DIFFERENT tree, so it sits outside the table above.
    res = compare_baseline({"s1": {"a.cpp": [10, 20]}}, base, source_names={"a.cpp"})
    good = bool(res["vanished_ok"]) and not res["vanished_red"]
    print("  %-28s %s" % ("a DELETED source is absorbed", "ok" if good else "FAIL"))
    bad += 0 if good else 1
    # ...and so does the one that measured `rebind_traps.gen.cpp`: still in the tree, nothing left in
    # it to instrument. Absorbed, and the arm asserts the OTHER direction too -- a file with real code
    # in it must stay red, or this branch would swallow every genuine build dropout.
    for label, empty, want_red in (
        ("an EMPTY source is absorbed", lambda n: True, False),
        ("a file WITH code stays red", lambda n: False, True),
    ):
        res = compare_baseline({"s1": {"a.cpp": [10, 20]}}, base, source_names=SRC, empty_src=empty)
        good = bool(res["vanished_red"]) == want_red
        print("  %-28s %s" % (label, "ok" if good else "FAIL"))
        bad += 0 if good else 1

    # THE OWNERSHIP SCOPE'S ARMS. The same drop, in the same file, must be RED when the domain owns
    # the file and ABSORBED when it does not -- and the absorbed one must still be COUNTED, because
    # a foreign drop that vanishes from the report entirely is how a domain stops noticing its
    # neighbours. Both directions, one input.
    fdrop = {"s1": {"a.cpp": [7, 20], "b.cpp": [5, 10]}}
    for label, own, red_n, foreign_n in (
        ("an OWN file's drop is red", {"a.cpp"}, 1, 0),
        ("a FOREIGN drop is counted", {"b.cpp"}, 0, 1),
        ("no census -> everything gated", None, 1, 0),
    ):
        res = compare_baseline(fdrop, base, source_names=SRC, owned=own)
        good = len(res["drops"]) == red_n and len(res["foreign_drops"]) == foreign_n
        print("  %-28s %s" % (label, "ok" if good else "FAIL"))
        bad += 0 if good else 1

    # THE ROW HALF'S OWN ARMS. Same reason as above and it bites harder here: compare_rows is the
    # stronger gate, and the easy bug in a set-difference comparison is a direction confusion that
    # makes the FAILING case never fire. So the going-cold arm is asserted to fail and the going-warm
    # arm asserted not to.
    rbase = {"s1": ["row_a", "row_b"]}
    CENSUS = {"row_a", "row_b", "row_c"}
    rcases = [
        ("rows identical is silent", rbase, None, None, lambda r: not any(r[k] for k in r)),
        (
            "a row GOING COLD fails",
            {"s1": ["row_a"]},
            None,
            None,
            lambda r: r["cold"] and not r["warm"],
        ),
        (
            "a row GOING WARM never fails",
            {"s1": ["row_a", "row_b", "row_c"]},
            None,
            None,
            lambda r: r["warm"] and not r["cold"],
        ),
        (
            "a NEW scenario never fails",
            {"s1": rbase["s1"], "s2": ["z"]},
            None,
            None,
            lambda r: r["added"] and not r["cold"],
        ),
        (
            "order does not matter",
            {"s1": ["row_b", "row_a"]},
            None,
            None,
            lambda r: not any(r[k] for k in r),
        ),
        # THE CENSUS-DRIFT ARMS (2026-09-11), from the shape measured at HEAD: the two rows that went
        # cold on this gate were `llm_tact_ui_char_panel_row_draw` and `_sidebar_row_draw_right`, both
        # DELETED by LIFT-TACT slice A (ee8fbeb0) along with their TUs. Nothing lost reach -- the rows
        # stopped existing, and the regenerated census has 96 where the baseline recorded 98. Failing
        # on that is failing on the instrument. The paired arm keeps the teeth: a row the census STILL
        # holds is red on exactly the same input.
        (
            "a row the census DROPPED",
            {"s1": ["row_a"]},
            CENSUS - {"row_b"},
            None,
            lambda r: r["census_dropped"] and not r["cold"],
        ),
        (
            "...but one it KEEPS is red",
            {"s1": ["row_a"]},
            CENSUS,
            None,
            lambda r: r["cold"] and not r["census_dropped"],
        ),
        (
            "an UNATTRIBUTED row is not red",
            {"s1": ["row_a"]},
            CENSUS,
            {"row_b"},
            lambda r: r["no_verdict"] and not r["cold"],
        ),
    ]
    for name, new, census, unatt, ok in rcases:
        res = compare_rows(new, rbase, census=census, unattributed=unatt)
        good = bool(ok(res))
        print("  %-28s %s" % (name, "ok" if good else "FAIL"))
        bad += 0 if good else 1

    # THE SPAN FINDER'S ARMS. It separates a definition from a declaration and from a call using one
    # test (a `{` after the parameter list), so each of those three shapes is asserted here -- plus the
    # case that motivated the shared stripper: a `/*` inside a `//` banner must not swallow the code
    # after it. All four shapes are real ones from src/mh_dll/mh.
    src = (
        "void other();\n"
        "void unit_tick();\n"  # a declaration -- must NOT be a span
        "namespace detail {\n"
        "void unit_tick(int a) {\n"  # a definition -- must be a span
        "    if (a) { helper(); }\n"
        "}\n"
        "}\n"
        "void caller() {\n"
        "    unit_tick(1);\n"  # a call -- must NOT be a span
        "}\n"
    )
    spans = _fn_spans(src, "unit_tick")
    span_ok = spans == [(4, 6)]
    print("  %-28s %s" % ("def yes, decl+call no", "ok" if span_ok else "FAIL %s" % (spans,)))
    bad += 0 if span_ok else 1

    # THE CORE/FLAKY SPLIT'S ARMS. The shape the real failure had is the third one: a line present in
    # SOME runs must land in flaky and must NOT be pinned in the core, because a core line is what the
    # gate demands of every later run. Getting that backwards is invisible for exactly as long as it
    # takes for a near-threshold branch to flip -- which on 2026-09-05 was one recording session, and
    # the committed baseline pinned three lines seven of nine runs never reached.
    lcases = [
        (
            "all runs agree -> all core",
            [{"a.cpp": [1, 2]}, {"a.cpp": [2, 1]}],
            {"a.cpp": [1, 2]},
            {},
        ),
        (
            "a line in SOME runs is flaky",
            [{"a.cpp": [1, 2, 3]}, {"a.cpp": [1, 2]}, {"a.cpp": [1, 2, 3]}],
            {"a.cpp": [1, 2]},
            {"a.cpp": [3]},
        ),
        (
            "a file only some runs saw",
            [{"a.cpp": [1]}, {}],
            {"a.cpp": []},
            {"a.cpp": [1]},
        ),
        ("one run -> nothing flaky", [{"a.cpp": [1, 2]}], {"a.cpp": [1, 2]}, {}),
    ]
    for name, runs, want_core, want_flaky in lcases:
        gc, gf = core_and_flaky_lines(runs)
        good = gc == want_core and gf == want_flaky
        print("  %-28s %s" % (name, "ok" if good else "FAIL core=%s flaky=%s" % (gc, gf)))
        bad += 0 if good else 1

    bad += _selftest_mutations()
    print("coverage-selftest: %s" % ("PASS" if not bad else "FAIL"))
    return 1 if bad else 0


def _selftest_mutations():
    """PROVE THE ARMS BITE, by breaking the rule and requiring the arms to go red.

    An arm that cannot fail is not an arm -- the recurring lesson of this tree, now at least five
    instruments deep (G106, G171, fptest case W, arm A of the world oracle, the routing census). It
    bites hardest on a DROPS-ONLY rule, whose every softening looks like the design: absorb one case
    too many and the gate still prints PASS on every input it is ever shown, which is exactly the
    state this gate was in when it was redesigned.

    So each mutation below replaces `classify_file_delta` with a plausible WRONG rule -- the ones a
    future edit would actually introduce -- and the pass requires the arm set to notice. The failure
    mode being excluded is not "the rule is wrong"; it is "the arms would not have told us".
    """
    real = globals()["classify_file_delta"]

    def _run_arms():
        """Re-run just the pure verdict table against whatever classify_file_delta is installed."""
        table = [
            (10, 20, 10, 20, "same"),
            (10, 20, 14, 20, "gain"),
            (10, 20, 9, 20, "drop"),
            (40, 44, 34, 34, "reshaped"),
            (10, 20, 8, 18, "reshaped"),
            (10, 20, 7, 18, "drop"),
            (10, 20, 9, 26, "drop"),
        ]
        return sum(
            1
            for o, ot, h, t, want in table
            if globals()["classify_file_delta"](o, ot, h, t)[0] != want
        )

    mutations = [
        # The softening that ends the gate: absorb every drop as a reshape.
        (
            "always absorb",
            lambda o, ot, h, t: ("reshaped" if h < o else ("gain" if h > o else "same"), 0, ""),
        ),
        # The opposite, and the rule this replaced: no allowance at all, every drop red.
        (
            "no allowance",
            lambda o, ot, h, t: ("drop" if h < o else ("gain" if h > o else "same"), 0, ""),
        ),
        # An unbounded allowance -- the off-by-one that makes the boundary arms vacuous.
        (
            "allowance unbounded",
            lambda o, ot, h, t: (
                "reshaped" if (h < o and ot != t) else real(o, ot, h, t)[0],
                0,
                "",
            ),
        ),
        # Direction inverted: a gain read as a loss.
        ("direction inverted", lambda o, ot, h, t: real(t, ot, h and o, ot)),
    ]
    bad = 0
    for name, mut in mutations:
        globals()["classify_file_delta"] = mut
        try:
            n = _run_arms()
        except Exception:
            n = 1  # a rule that throws is also caught -- that is the arms noticing
        finally:
            globals()["classify_file_delta"] = real
        print(
            "  mutation %-19s %s"
            % (name, "ok (%d arm(s) red)" % n if n else "FAIL -- arms stayed green")
        )
        bad += 0 if n else 1
    n = _run_arms()
    print("  %-28s %s" % ("restored rule is green", "ok" if n == 0 else "FAIL %d red" % n))
    return bad + (0 if n == 0 else 1)


def measure_sim_one(sc, occ, args):
    """Run ONE registered sim scenario (an all-AI soak) under the collector.

    THE COLLECTOR WRAPS test_ui.py, NOT THE GAME, and that is measured rather than assumed. The
    tactical path can hand OpenCppCoverage the game command line directly because _rig_cmd provisions
    the lane itself; a soak cannot -- it reaches its 8-way match by driving the real menu through
    ui_test.py, which launches the game via PowerShell Start-Process. So the collector runs with
    `--cover_children` and follows the tree. Verified 2026-09-05: a 300-step soak produced a 1.87 MB
    report naming 371 sim_* files, with sim_step.cpp at 126/146 lines. Reimplementing the lane
    provisioning here to get a direct launch would have duplicated the menu drive for no gain.
    """
    xml_path = os.path.join(COV_DIR, "sim_%s.xml" % sc["name"])
    os.makedirs(COV_DIR, exist_ok=True)
    if os.path.exists(xml_path):
        os.remove(xml_path)
    dll = os.path.join(REPO, "src", "mh_dll", "Debug", "mh.dll")
    if getattr(args, "debug_dll", True):
        if not build_debug_dll():
            return None
    steps = int(getattr(args, "steps", 0) or sc.get("steps", 600))
    inner = [
        sys.executable,
        os.path.join(REPO, "tools", "test_ui.py"),
        "--soak",
        "--steps",
        str(steps),
    ]
    # --steps AND --timeout BOTH have to reach the inner soak, and neither did. test_ui derives its own
    # timeout as max(--timeout, 240 + steps/40) -- 990 s at 30k steps -- which is generous for a Release
    # run and nowhere near enough under the collector with an UNOPTIMISED dll. A truncated run is not a
    # loud failure here: it produces a perfectly good coverage report for however far it got, so the
    # number would simply be quietly low. Pass both through, and give the outer subprocess room above
    # whatever the inner one is allowed.
    if getattr(args, "timeout", 0):
        inner += ["--timeout", str(int(args.timeout))]
    if getattr(args, "debug_dll", True):
        inner += ["--soak-dll", dll]
    spd = int(getattr(args, "speed", 0) or sc.get("speed", 0) or 0)
    if spd:
        inner += ["--soak-speed", str(spd)]
    if sc.get("save"):
        inner += ["--soak-save", sc["save"], "--soak-load-at", str(sc.get("load_at", 120))]
    for frag in list(sc.get("extra_ini", ())) + list(getattr(args, "extra_ini", ()) or []):
        inner += ["--extra-ini", frag]
    occ_args = [
        occ,
        "--quiet",
        "--cover_children",
        "--sources",
        os.path.abspath(args.sources),
        "--export_type",
        "cobertura:%s" % xml_path,
        "--",
    ] + inner
    r = subprocess.run(
        occ_args,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=max(3600, int(getattr(args, "timeout", 0) or 0) + 900),
    )
    # THE SOAK'S SHAPE VERDICT IS REPORTED AND DOES NOT GATE THE MEASUREMENT -- BUT AN INVALID RUN
    # DOES. Those are different things and the distinction is the whole lesson of 2026-09-05.
    #
    # A soak may FAIL its shape rules (nobody eliminated, too quiet, not all-AI after a save load) and
    # still be a truthful record of what executed; coverage is coverage. So those failures are printed
    # and ignored here.
    #
    # A run the DLL declares INVALID is not that. `N/M seams installed -- PARTIAL, treat this run as
    # invalid` means the promotion set it was asked for is not the set that installed, so the run
    # measured a configuration nobody chose -- and a baseline recorded from it is a record of the wrong
    # thing. Two baselines were recorded exactly that way before this refusal existed (all-AI soaks
    # with [promote] sim_resid=1, which cannot install its landing seam because the ALLAI detour owns
    # that entry -- 30/31, every time). REFUSE rather than warn: a warning in a 3-run baseline loop is
    # a line nobody reads.
    invalid = []
    for ln in (r.stdout or "").splitlines():
        t = ln.strip()
        # `hashed steps: N of M requested` is included DELIBERATELY: a coverage run that ended early
        # still produces a perfectly good report for however far it got, so a truncated deep run reports
        # a quietly LOW number with nothing saying why. The lane is rmtree'd by the next scenario, so
        # this line is the only surviving evidence of how far the run actually reached.
        if t.startswith(("soak:", "all_ai:", "FAIL:", "AI:", "hashed steps:")) or t.startswith(
            ("      all_ai:", "      FAIL:", "      AI:", "      hashed steps:")
        ):
            print("   %s" % t[:130])
        if "declared this run INVALID" in t or "treat this run as invalid" in t:
            invalid.append(t[:160])
    if invalid:
        print("   REFUSED: this scenario's configuration does not install cleanly --")
        for t in invalid[:3]:
            print("      %s" % t)
        print(
            "      A coverage baseline off an invalid run records a configuration nobody chose. "
            "Since fork F2E a scenario has no per-key promote config to fix: which bodies run is "
            "`[config] mode`, so a PARTIAL install is a DLL-side refusal, not a knob."
        )
        return None
    if not os.path.isfile(xml_path):
        for ln in (r.stderr or "").splitlines()[-6:]:
            print("      %s" % ln[:110])
        return None
    # STASH THE REACHED STEP COUNT beside the report. The soak prints `hashed steps: N of M requested`;
    # the lane that holds the log is rmtree()d by the next scenario, and the report itself does not know
    # how far the run got -- so without this a rung labelled 30000 that actually ran 11000 flattens the
    # depth curve and reads as saturation. See _reached_steps.
    #
    # IT BELONGS HERE, IN THE SOAK PATH, and the first attempt put it in measure_one -- the TACTICAL
    # journal path -- because the anchor text it was spliced against appears in both functions and the
    # replace took the first. Nothing broke; the sidecar was simply never written, and the whole first
    # depth curve came out reading "reached ?" on every rung. A journal replay has no `hashed steps`
    # line at all, so it would never have written one there either.
    m = re.search(r"hashed steps:\s*(\d+)\s*of", r.stdout or "")
    if m:
        with open(os.path.splitext(xml_path)[0] + ".steps", "w", encoding="utf-8") as fh:
            fh.write(m.group(1))
    return xml_path


def _reached_steps(xml_path):
    """How far the run that produced this report actually got, from the soak's own line.

    measure_sim_one stashes it beside the report because the LANE IS DESTROYED by the next scenario --
    make_lane rmtree()s it, so mh_harness.log is gone within minutes and the only surviving record of
    the reached step count is whatever was captured at the time. Returns None when unknown, and None is
    printed as "?" rather than silently as a number.
    """
    side = os.path.splitext(xml_path)[0] + ".steps"
    try:
        with open(side, encoding="utf-8") as fh:
            return int(fh.read().strip())
    except (OSError, ValueError):
        return None


def run_milestones(args, occ):
    """Coverage at a LADDER of step budgets, so the depth curve is measured rather than assumed.

    WHY A RUN PER BUDGET. OpenCppCoverage writes its report when the process EXITS -- there is no
    mid-run snapshot to take, and no amount of wanting one produces it. So a curve costs one run per
    rung. That is the honest price of the question "does 30k buy anything over 5k", and the question is
    worth it: measured 2026-09-05, going 600 -> 30000 steps moved the sim union 53% -> 77% and reached 95
    files that no amount of re-running 600 steps would ever enter.

    IT NEVER WRITES THE GATED BASELINE, and that is a fix rather than a nicety. The 30k probe was run as
    `--baseline --steps 30000` and it OVERWROTE tools/data/sim_coverage.json with a single-run record --
    replacing an intersection-of-3 core with exactly the one-sample core that core exists to avoid. The
    committed baseline had to be restored from git. A curve is a measurement, so it lands in tmp/.

    EACH RUNG REPORTS THE STEPS IT ACTUALLY REACHED, not the budget it was given. A soak can end early
    (wall clock, or the match resolving) and still produce a perfectly good report for however far it
    got -- so a rung labelled 30000 that ran 11000 would otherwise flatten the curve and read as
    saturation.
    """
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import test_ui

    domain = getattr(args, "domain", "sim")
    scens = getattr(test_ui, "%s_SCENARIOS" % domain.upper(), None)
    if not scens:
        print("FAIL: no %s_SCENARIOS registry in test_ui.py" % domain.upper())
        return 1
    rungs = [int(x) for x in str(args.milestones).replace(" ", "").split(",") if x]
    args.debug_dll = True
    curve, rc = {}, 0
    for steps in sorted(rungs):
        for sc in scens:
            print("=" * 78)
            print("milestone %d steps -- %s/%s" % (steps, domain, sc["name"]))
            print("=" * 78)
            sub = argparse.Namespace(**vars(args))
            sub.steps = steps
            xml = measure_sim_one(sc, occ, sub)
            if xml is None:
                print("FAIL: %s at %d steps produced no report" % (sc["name"], steps))
                rc = 1
                continue
            ran, cold, un = attribute(xml, domain)
            reached = _reached_steps(xml)
            curve.setdefault(sc["name"], []).append(
                {
                    "budget": steps,
                    "reached": reached,
                    "ran": sorted(ran),
                    "n_ran": len(ran),
                    "n_cold": len(cold),
                    "n_unattributed": len(un),
                }
            )
            print(
                "  rows      %d RAN / %d cold / %d unattributed   (budget %d, reached %s)"
                % (len(ran), len(cold), len(un), steps, reached if reached is not None else "?")
            )

    print("=" * 78)
    print("DEPTH CURVE -- %s" % domain)
    for name, rows in sorted(curve.items()):
        print("  %s" % name)
        prev = set()
        for r in rows:
            new = set(r["ran"]) - prev
            print(
                "     budget %-6d reached %-7s %3d rows  (+%d new over the rung below)"
                % (
                    r["budget"],
                    r["reached"] if r["reached"] is not None else "?",
                    r["n_ran"],
                    len(new),
                )
            )
            prev |= set(r["ran"])
    union = {}
    for name, rows in curve.items():
        for r in rows:
            union.setdefault(r["budget"], set()).update(r["ran"])
    print("  UNION over the registered set, per rung:")
    for b in sorted(union):
        print("     budget %-6d %3d row(s)" % (b, len(union[b])))
    out = os.path.join(REPO, "tmp", "coverage_curve_%s.json" % domain)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(
            {
                "_generated_by": "tools/coverage.py --milestones",
                "_measures": "rows executed per SCENARIO per step BUDGET, with the steps each run "
                "actually reached. A measurement, never a gate -- the committed baseline is written "
                "only by --baseline, deliberately.",
                "domain": domain,
                "curve": curve,
            },
            f,
            indent=1,
            sort_keys=True,
        )
        f.write("\n")
    print("wrote %s" % out)
    return rc


def run_combined(args):
    """UNION every committed per-domain baseline: what does the WHOLE corpus execute?

    The per-domain records answer "what did THIS family of scenarios reach". Nobody was answering the
    question a reader actually has -- what fraction of our C++ do we execute AT ALL -- and it is not
    obtainable by adding the per-domain figures up: a line reached by both a tactical journal and the
    campaign is one covered line, not two. The records carry per-FILE covered LINE NUMBERS (added
    2026-09-05 so a regression could name lines), so the union is exact and costs no rig time.

    THE COLUMN THAT DECIDES THINGS IS `unique`, not `hit`. A scenario's own coverage is mostly shared
    with its siblings; what justifies its minutes is what it reaches that NOTHING ELSE does. That is
    the number to look at when asking whether a fixture earns its place in the gate.

    A SOURCE-DRIFT GUARD, because combining records taken on different days is exactly where a
    silently wrong total comes from: if two records disagree about a file's TOTAL line count, the tree
    moved under one of them and the combination is reported as SUSPECT rather than quietly averaged.
    """
    recs = sorted(glob.glob(os.path.join(REPO, "tools", "data", "*_coverage.json")))
    recs = [r for r in recs if not r.endswith("combined_coverage.json")]
    if not recs:
        print("FAIL: no tools/data/<domain>_coverage.json baselines to combine")
        return 1
    union = collections.defaultdict(set)
    totals, per_scen, drift, stamps, no_lines = {}, {}, [], [], []
    for r in recs:
        dom = os.path.basename(r).replace("_coverage.json", "")
        d = json.load(open(r, encoding="utf-8"))
        stamps.append((dom, os.path.getmtime(r), len(d.get("scenarios", {}))))
        for name, sc in d.get("scenarios", {}).items():
            lines = {f: set(v) for f, v in (sc.get("lines") or {}).items()}
            # A RECORD WITHOUT PER-LINE DATA CANNOT JOIN A UNION, and listing it at 0.0% reads as a
            # scenario that covers nothing rather than one that was recorded before the `lines` field
            # existed (2026-09-05). sim_resid is exactly that. Name them instead of scoring them.
            if not lines:
                no_lines.append("%s/%s" % (dom, name))
                continue
            per_scen["%s/%s" % (dom, name)] = lines
            for f, hv in (sc.get("files") or {}).items():
                if not isinstance(hv, list) or len(hv) < 2:
                    continue
                if f in totals and totals[f] != hv[1]:
                    drift.append((f, totals[f], hv[1]))
                totals[f] = max(totals.get(f, 0), hv[1])
            for f, ls in lines.items():
                union[f] |= ls

    hit = sum(len(v) for v in union.values())
    tot = sum(totals.values())
    print("=" * 78)
    print("COMBINED baseline -- the union of every registered scenario")
    print("=" * 78)
    for dom, mt, n in stamps:
        print(
            "  %-12s %d scenario(s)   recorded %s"
            % (dom, n, time.strftime("%Y-%m-%d %H:%M", time.localtime(mt)))
        )
    print()
    print(
        "  COMBINED  %d/%d lines = %d%%  across %d file(s), %d scenario(s)"
        % (hit, tot, (100 * hit // tot) if tot else 0, len(totals), len(per_scen))
    )
    print()
    print("  %-26s %10s %10s   %s" % ("scenario", "hit", "unique", "% of union"))
    owned = collections.Counter()
    for name, lines in per_scen.items():
        for f, ls in lines.items():
            for n in ls:
                owned[(f, n)] += 1
    rows = []
    for name, lines in per_scen.items():
        own = sum(len(v) for v in lines.values())
        uniq = sum(1 for f, ls in lines.items() for n in ls if owned[(f, n)] == 1)
        rows.append((own, uniq, name))
    for own, uniq, name in sorted(rows, reverse=True):
        print("  %-26s %10d %10d   %5.1f%%" % (name, own, uniq, 100.0 * own / hit if hit else 0))

    if no_lines:
        print()
        print(
            "  NO PER-LINE DATA, so excluded from the union (recorded before the `lines` field): %s"
            % ", ".join(no_lines)
        )

    cold = sorted(f for f, t in totals.items() if t and not union.get(f))
    print()
    print("  NEVER ENTERED by ANY scenario: %d file(s) of %d" % (len(cold), len(totals)))
    for f in cold[:15]:
        print("     %s" % f)
    if len(cold) > 15:
        print("     ... and %d more" % (len(cold) - 15))

    if drift:
        print()
        print(
            "  RECORDS DISAGREE on files' total line counts (%d file(s)) -- so they were NOT all"
            % len(drift)
        )
        print(
            "  measured the same way, and this combination is SUSPECT. TWO causes, and the second"
        )
        print(
            "  is the one that has actually happened here: (a) the records were taken on different"
        )
        print(
            "  source revisions; (b) one of them measured an OPTIMISED dll, where /O2+LTCG inlines"
        )
        print(
            "  small bodies out of existence so their lines never reach the PDB. (b) shows as one"
        )
        print("  record being a strict SUBSET with uniformly FEWER lines per file -- never more.")
        for f, a, b in drift[:8]:
            print("     %-46s %d vs %d" % (f, a, b))
        print("  A Release-measured record makes its own coverage look SMALLER, so the combined")
        print("  figure is a LOWER BOUND rather than merely uncertain. KNOWN 2026-09-08: the sim")
        print("  path measures an optimised dll (its soak lane gets the Release build despite")
        print("  --soak-dll). tact and uirec are Debug and comparable.")

    out = os.path.join(REPO, "tools", "data", "combined_coverage.json")
    json.dump(
        {
            "_generated_by": "tools/coverage.py --combined",
            "_measures": "the UNION of every tools/data/<domain>_coverage.json, per file. `unique` "
            "is what one scenario covers that NO other does -- the number that says whether a "
            "fixture earns its rig time.",
            "_sources": [os.path.basename(r) for r in recs],
            "combined": {"hit": hit, "total": tot, "files": len(totals)},
            "per_scenario": {n: {"hit": o, "unique": u} for o, u, n in rows},
            "never_entered": cold,
            "no_line_data": no_lines,
            "drift": [{"file": f, "a": a, "b": b} for f, a, b in drift],
        },
        open(out, "w", encoding="utf-8"),
        indent=1,
        sort_keys=True,
    )
    print()
    print("wrote %s" % out)
    return 0


def run_baseline(args, occ):
    """Measure every registered scenario of the domain and write (or check) the committed record."""
    domain = getattr(args, "domain", "tact")
    args.debug_dll = True  # a baseline off an optimised build records inlining as real zeroes
    path = baseline_path(domain)
    record = {}

    # MEASURE ONCE, COMPARE MANY. A tact pass is four instrumented journal replays plus a Debug
    # build; a sim pass is worse. Before this, every iteration on the COMPARISON rule -- which is
    # pure data -- cost a full pass, so the rule got tuned by guesswork between measurements. The
    # dumped record is exactly what --check compares, so `--from-measured` replays a red offline,
    # bit for bit, weeks later. Not a gate input: it never writes the committed baseline.
    fm = getattr(args, "from_measured", None)
    if fm:
        record = json.load(open(fm, encoding="utf-8"))["scenarios"]
        print("measured record REPLAYED from %s (%d scenario(s)) -- no run" % (fm, len(record)))
        if not args.check:
            print("FAIL: --from-measured is for --check only (it must never write the baseline)")
            return 1
        targets = []
    elif domain == "tact":
        journals = registered_journals()
        if not journals:
            print("FAIL: no registered tactical scenarios found in test_ui.py")
            return 1
        targets = [(os.path.basename(r).replace(".journal", ""), r) for r in journals]
    elif domain == "uirec":
        sys.path.insert(0, os.path.join(REPO, "tools"))
        import test_ui

        # THE PER-ROW HALF DOES NOT APPLY HERE, and its output says so in a way that reads like a
        # defect: `rows run 1: 0 RAN, 0 cold, 1 unattributed`. That is correct, not a miss. Row
        # attribution keys off `<domain>_liveness.json`, which exists for the MIGRATION domains (sim,
        # tact) because a row there is a translated function with a liveness route. `uirec` is not a
        # migration domain -- it is a SCENARIO -- so there is no row census to attribute against and
        # the per-FILE table is the whole answer. Do not "fix" the zero by generating a liveness file
        # for it; run --domain sim/tact if the question is which rows a session reached.
        #
        # Only what the registry marks measurable -- see UIREC_SCENARIOS' `coverage` key.
        scens = [sc for sc in test_ui.UIREC_SCENARIOS if sc.get("coverage")]
        if not scens:
            print("FAIL: no UIREC_SCENARIOS entry carries `coverage: True`")
            return 1
        targets = [(sc["name"], sc["journal"]) for sc in scens]
    else:
        sys.path.insert(0, os.path.join(REPO, "tools"))
        import test_ui

        scens = getattr(test_ui, "%s_SCENARIOS" % domain.upper(), None)
        if not scens:
            print("FAIL: no %s_SCENARIOS registry in test_ui.py" % domain.upper())
            return 1
        targets = [(sc["name"], sc) for sc in scens]

    for name, spec in targets:
        print("=" * 78)
        print("baseline scenario: %s/%s" % (domain, name))
        print("=" * 78)
        if domain in ("tact", "uirec"):
            sub = argparse.Namespace(**vars(args))
            sub.journal = os.path.join(REPO, spec)
            sub.baseline = sub.check = False
            sub.uirec = domain == "uirec"
            xml = measure_one(sub, occ)
        else:
            xml = measure_sim_one(spec, occ, args)
        # REPEATED, AND THE STORED SET IS THE INTERSECTION. Measured 2026-09-05, which is the only
        # reason this loop exists: two runs of the IDENTICAL soak_saved command line covered 214 and
        # then 223 rows, and the nine extra ones are random-event driven --
        # llm_strat_invasion_chance_roll, llm_strat_invasion_alert_arm, game_HandleInvasion,
        # llm_strat_revoke_invention, llm_strat_mother_reelect_primary. So a row CAN come and go
        # between runs even under the soak's pinned wallclock, and a gate that failed whenever a row
        # went cold would be flaky -- which means it would be switched off inside a week, taking the
        # strong half of this check with it.
        #
        # The intersection is the REPRODUCIBLE CORE: a row in it ran in every recorded run, so a later
        # run losing it is a signal rather than a coin flip. Rows seen in only some runs are kept
        # separately as `rows_flaky` -- reported, never gated on. This is the same shape as
        # compare_baseline's regression-only rule: pin what is stable, report what moves.
        #
        # THE CORE IS ONLY AS GOOD AS N, and two samples agreeing is NOT determinism -- a later pair of
        # runs at --runs 2 agreed exactly (143/143 and 223/223, zero flaky) on the same scenarios whose
        # earlier pair differed by nine rows, and a reformat was RULED OUT as the cause (zero source
        # files changed). So the variance is real and intermittent, which is the worst kind for a gate.
        # Hence the default of 3 rather than 2. If --check ever fails on a row that turns out to come
        # and go, the fix is to re-record with a higher --runs, not to soften the comparison: the whole
        # value of the row half is that a cold row is a real signal.
        # tact and uirec stay at ONE run: their scenarios replay a RECORDED JOURNAL, which is
        # reproducible by construction, and tact's committed baseline predates this field. Repetition
        # is for the soaks, whose variance is random-event driven. uirec's reproducibility is not an
        # assumption either -- SPCAMP-FLAKE closed on twenty consecutive replays of this very journal
        # producing byte-identical per-step trajectories.
        runs = (
            1
            if (domain in ("tact", "uirec") or args.check)
            else max(1, int(getattr(args, "runs", 2) or 2))
        )
        per_run, files, per_run_lines = [], {}, []
        for i in range(runs):
            x = xml if i == 0 else measure_sim_one(spec, occ, args)
            if x is None:
                print("FAIL: %s run %d produced no coverage report" % (name, i + 1))
                return 1
            files = summarise(x, focus=()) if i == 0 else files
            # PER-LINE, so a regression can name the LINES and not just a count (added 2026-09-05).
            # The baseline stored `[hit, total]` per file and nothing else, so the gate could say
            # "tact_frame.cpp 223 -> 221" and never which two -- which is most of why bisecting the
            # TACT-COV-YIELD drop took a day and four rejected hypotheses. Measured cost of keeping the
            # covered-line numbers for tact's 696 files: 0.09 MB against a 0.02 MB baseline.
            per_run_lines.append(hit_lines(x))
            ran_i, cold_i, unattributed = attribute(x, domain)
            per_run.append(set(ran_i))
            print(
                "  rows      run %d: %d RAN, %d cold, %d unattributed (of %d verified)"
                % (
                    i + 1,
                    len(ran_i),
                    len(cold_i),
                    len(unattributed),
                    len(ran_i) + len(cold_i) + len(unattributed),
                )
            )
        core = set.intersection(*per_run)
        flaky = set.union(*per_run) - core
        if flaky:
            print(
                "  rows      %d row(s) ran in SOME runs but not all -- reported, never gated"
                % len(flaky)
            )
        # THE LINE SETS ARE THE INTERSECTION TOO, and the per-file COUNT is derived from them so the two
        # halves of this record cannot disagree. Before this the count came from run 1 while the rows
        # came from the intersection, which was already a small inconsistency; keeping a line LIST from
        # run 1 next to it would have been a real one -- a reader diffing the list would get a different
        # answer from the number beside it. Same doctrine as the row core: pin what reproduces.
        #
        # AND THE FLAKY LINES ARE KEPT TOO, for the reason the row half already had a `rows_flaky`
        # bucket: without it, a line that comes and goes is INVISIBLE in the record, so the next
        # --check failure on one looks like a fresh regression and gets bisected as one. That cost a
        # day on 2026-09-05. The four lines involved -- ai_worker_rebalance.cpp 269/271/273 and
        # ai_construction_plan.cpp 113 -- are ONE branch (`if (1.0 <= ai_labor_utilization) return 0`
        # at :268, and 113 sits under its returned housing_short), and the recorded history is:
        #
        #   93b92c0b record (3 runs)  cold      e3692992 record (3 runs)  WARM
        #   --check after it (1 run)  cold      2 bare A/A runs           cold
        #   2 collector A/A runs      cold
        #
        # i.e. ONE recording run of five saw them, and the committed baseline is that run --
        # so the core contains three lines seven of nine runs never reach, and --check is red forever.
        # It is not the instrument and it is not the fixture drifting: the A/A goldens matched over all
        # 600 steps INCLUDING the pN_ai_econ slice that holds ai_labor_utilization (+0x104b8), and the
        # collector's golden is byte-identical to the un-instrumented one, so OpenCppCoverage does not
        # perturb the sim. Runs minutes apart agree; recordings hours apart do not, which points at an
        # unpinned per-session input -- pin_wallclock replaces exactly ONE function (GetCurrentTime),
        # so anything else reading real time is uncovered. UNPROVEN, and named as a lead only.
        #
        # Recorded, never gated -- same rule as rows_flaky. The gate keeps its teeth because the core
        # is what it compares; this bucket is so the NEXT one is diagnosed from the file in a minute.
        lines_core, lines_flaky = core_and_flaky_lines(per_run_lines)
        if lines_flaky:
            print(
                "  lines     %d line(s) across %d file(s) ran in SOME runs but not all -- reported, "
                "never gated" % (sum(len(v) for v in lines_flaky.values()), len(lines_flaky))
            )
        files_out = {}
        for k, v in sorted(files.items()):
            hit, tot = list(v)[0], list(v)[1]
            files_out[k] = [len(lines_core[k]), tot] if k in lines_core else [hit, tot]
        record[name] = {
            "files": files_out,
            "lines": lines_core,
            "lines_flaky": lines_flaky,
            "rows_ran": sorted(core),
            "rows_flaky": sorted(flaky),
            "runs": len(per_run),
        }

    # THE UNION OVER THE REGISTERED SET, WITH THE GAP NAMED (SIM1-P clause 9). The per-scenario figure
    # is the one that gets misread: on TACT1-P the per-journal "CANNOT TEST" lines were read as gaps
    # when two different fixtures between them covered the op. A union is the reportable number and the
    # gap count is stated beside it, because the clause asks for both to be COMPUTED, never for the set
    # to be complete.
    if len(record) > 1:
        sets = {k: set(v["rows_ran"]) for k, v in record.items()}
        union = set().union(*sets.values())
        spans, unattributed = row_spans(domain)
        total = len(spans) + len(unattributed)
        print("=" * 78)
        print("UNION over %d registered %s scenario(s)" % (len(record), domain))
        for k, v in sorted(sets.items()):
            uniq = v - set().union(*(s for n, s in sets.items() if n != k))
            print("   %-16s %4d rows  (%d reached by NO other scenario)" % (k, len(v), len(uniq)))
        print(
            "   %-16s %4d of %d verified rows = %.0f%%;  GAP %d rows never executed by any of them"
            % (
                "UNION",
                len(union),
                total,
                100.0 * len(union) / max(total, 1),
                len(spans) - len(union),
            )
        )
        if unattributed:
            print(
                "   %-16s %4d rows carry NO verdict either way (not counted as covered OR as gap)"
                % ("unattributed", len(unattributed))
            )

    dm = getattr(args, "dump_measured", None)
    if dm and not fm:
        os.makedirs(os.path.dirname(os.path.abspath(dm)), exist_ok=True)
        with open(dm, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"_measured_only": True, "scenarios": record}, f, indent=1, sort_keys=True)
        print("dumped the measured record to %s (replay it with --from-measured)" % dm)

    if args.check:
        if not os.path.isfile(path):
            print("FAIL: no committed baseline at %s -- run without --check first." % path)
            return 1
        old = json.load(open(path, encoding="utf-8"))["scenarios"]
        print(BANNER)
        disp = load_dispositions(domain)
        # A MISSING CENSUS MUST NOT SOFTEN THE ROW HALF. row_spans() answers a domain with no
        # `<domain>_liveness.json` with a single `(no census)` placeholder, and feeding that in as the
        # census would put EVERY baseline row in the absorbed `census_dropped` bucket -- the row gate
        # switched off by an absent file, silently, which is the exact failure mode this redesign is
        # meant to remove rather than introduce. `None` keeps the strict comparison.
        cspans, cunatt = row_spans(domain)
        have_census = bool(cspans) or not any(n == "(no census)" for n, _w in cunatt)
        res = compare_baseline(
            {k: v["files"] for k, v in record.items()},
            {k: _files_of(v) for k, v in old.items()},
            dispositions=disp,
            source_names=_source_basenames(),
            owned=domain_owned_files(domain),
            empty_src=_has_instrumentable_code_not,
        )
        owned_n = domain_owned_files(domain)
        print(
            "  scope     %s"
            % (
                "%d of this domain's own TUs are gated; the other %d file(s) in the record are "
                "FOREIGN (reported, not gated)"
                % (
                    len({f for s in record.values() for f in s["files"] if f in owned_n}),
                    len({f for s in record.values() for f in s["files"]} - owned_n),
                )
                if owned_n
                else "no %s_liveness.json census -- EVERY file in the record is gated" % domain
            )
        )
        rows = compare_rows(
            {k: v["rows_ran"] for k, v in record.items()},
            {k: (v.get("rows_ran", []) if isinstance(v, dict) else []) for k, v in old.items()},
            census=(set(cspans) | {n for n, _w in cunatt}) if have_census else None,
            unattributed={n for n, _w in cunatt} if have_census else None,
        )
        regs = res["drops"]
        gains = res["gains"]
        added = res["added"]
        for scen, name in rows["cold"]:
            print("FAIL: %s/%s STOPPED RUNNING (row was covered, now cold)" % (scen, name))
        for key, why in res["vanished_red"]:
            print("FAIL: %s VANISHED from the report -- %s" % (key, why))
        for scen, name, ohit, otot, hit, tot, note in regs:
            print(
                "FAIL: %s/%s covered lines %d -> %d (of %d, was %d)"
                % (scen, name, ohit, hit, tot, otot)
            )
            print("        %s" % note)
            # NAME THE LINES. A count says a scenario lost reach; the line numbers say WHERE, which is
            # the difference between one diff and a day of bisecting (TACT-COV-YIELD). Older baselines
            # predate the `lines` map, so say that rather than printing nothing and looking complete.
            was = (old.get(scen) or {}).get("lines", {}) if isinstance(old.get(scen), dict) else {}
            now = record.get(scen, {}).get("lines", {})
            if tot != otot:
                # LINE NUMBERS ARE NOT SHIFT-TOLERANT and this is the one place that matters. The
                # file's instrumented population moved, so every recorded number below the edit is
                # off by the edit's size and the set difference would name dozens of "cold" lines
                # that simply renumbered. Suppress it rather than print a plausible lie: the counts
                # above are the comparable evidence, and `git diff` is the tool for the rest.
                print(
                    "        (no line detail: the file was RESHAPED %d -> %d instrumented lines, so "
                    "recorded line numbers no longer line up)" % (otot, tot)
                )
            elif not was:
                print(
                    "        (no line detail: this baseline predates the `lines` map -- re-record to "
                    "get the exact lines on the next regression)"
                )
            else:
                gone = sorted(set(was.get(name, ())) - set(now.get(name, ())))
                back = sorted(set(now.get(name, ())) - set(was.get(name, ())))
                if gone:
                    print("        WENT COLD: %s" % _ranges(gone))
                    # IF THE RECORDING ALREADY SAW THEM COME AND GO, SAY SO HERE. This is the whole
                    # payoff of lines_flaky: the reader learns in one line that the baseline's own runs
                    # disagreed about these lines, instead of opening a regression hunt. Still a FAIL --
                    # a line in the core is meant to reproduce, and the honest fix is to re-record at a
                    # higher --runs so it lands in the flaky bucket where it belongs.
                    kf = ((old.get(scen) or {}).get("lines_flaky", {}) or {}).get(name, ())
                    hit = sorted(set(gone) & set(kf))
                    if hit:
                        print(
                            "        ...of which %s were ALREADY RECORDED AS FLAKY -- the baseline's "
                            "own runs disagreed. Re-record at a higher --runs rather than bisecting."
                            % _ranges(hit)
                        )
                if back:
                    print("        newly covered (not a failure): %s" % _ranges(back))

        # ABSORBED, WITH A COUNT AND A SAMPLE. Everything below this line is information: it moved in
        # the direction the gate does not police. Printed as counts first because the volume is the
        # point -- a tact journal drives the whole DLL, so most entries are sim/ai/lib files whose
        # movement has nothing to do with this domain's question, and three gate readings running
        # were dominated by exactly that. A capped sample follows each count so the reader can see
        # WHAT moved without scrolling past a hundred lines to reach the verdict.
        def _sample(label, items, fmt, cap=6):
            if not items:
                return
            print("  %-10s %d" % (label, len(items)))
            for it in items[:cap]:
                print("               %s" % fmt(it))
            if len(items) > cap:
                print("               ... and %d more" % (len(items) - cap))

        _sample(
            "FOREIGN-DROP",
            res["foreign_drops"],
            lambda t: (
                "%s/%s %d -> %d (of %d, was %d) -- %s" % (t[0], t[1], t[2], t[4], t[5], t[3], t[6])
            ),
        )
        _sample("now-warm", rows["warm"], lambda t: "%s/%s" % t)
        _sample(
            "gain",
            gains,
            lambda t: "%s/%s %d -> %d (of %d, was %d)" % (t[0], t[1], t[2], t[4], t[5], t[3]),
        )
        _sample(
            "reshaped",
            res["reshaped"],
            lambda t: "%s/%s %d -> %d -- %s" % (t[0], t[1], t[2], t[4], t[6]),
        )
        # HOW MUCH OF THE ALLOWANCE THE ABSORPTIONS ACTUALLY USED -- the one number that says how
        # much this gate is choosing not to see. Measured at HEAD: 26 of 31 of this domain's own
        # reshapes sat EXACTLY at the ceiling, which is what a pure deletion of covered code looks
        # like (LIFT-TACT lifted whole bodies out) -- and is also what a deletion of the same size
        # PLUS lost reach looks like. The rule cannot separate those, so it says so instead.
        ceiling = [
            t for t in res["reshaped"] if (t[2] - t[4]) == max(0, t[3] - t[5]) and t[2] > t[4]
        ]
        if ceiling:
            print(
                "  at-ceiling %d of %d reshape(s) absorbed a drop EXACTLY equal to the lines their "
                "edit removed -- consistent with every removed line having been covered, and equally "
                "consistent with reach lost behind a deletion of the same size. Not separable by "
                "counts; `git diff` the file if it matters." % (len(ceiling), len(res["reshaped"]))
            )
        _sample(
            "stale",
            res["stale"],
            lambda t: "%s/%s %s" % (t[0], t[1], t[6]),
        )
        _sample("new", added + rows["added"], lambda s: s)
        _sample("gone-ok", res["vanished_ok"], lambda t: "%s -- %s" % t)
        _sample("row-gone", rows["census_dropped"], lambda t: "%s/%s no longer in the census" % t)
        _sample("row-noverdict", rows["no_verdict"], lambda t: "%s/%s unattributed this run" % t)

        bad = bool(regs or rows["cold"] or res["vanished_red"])
        print(
            "  totals    GATED: %d own-file drop(s), %d cold row(s), %d vanished.  ABSORBED: "
            "%d foreign drop(s), %d gain(s), %d reshaped, %d new, %d now-warm"
            % (
                len(regs),
                len(rows["cold"]),
                len(res["vanished_red"]),
                len(res["foreign_drops"]),
                len(gains),
                len(res["reshaped"]),
                len(added),
                len(rows["warm"]),
            )
        )
        print("coverage-baseline(%s): %s" % (domain, "FAIL" if bad else "PASS"))
        if bad:
            return 1
        # ABSORB ON GREEN, so the baseline tracks reality instead of ageing into one. A ratchet that
        # only ever moves up has to actually MOVE UP: the gate's whole failure history is a record
        # falling behind the tree until an entry became impossible against it (`40 -> 34 (of 34)`),
        # and every one of those reds cost an investigation. A green run has, by definition, lost no
        # coverage, so there is nothing a re-record can hide -- the only thing it discards is the
        # stale half. Refusals, deliberately: never under --from-measured (a replayed record is not
        # a measurement of the current tree), never when the run measured FEWER scenarios than the
        # baseline holds (a partial run would silently delete the rest), and never with --no-absorb.
        if getattr(args, "no_absorb", False):
            print(
                "  absorb    skipped (--no-absorb): the committed baseline still holds the old numbers"
            )
        elif fm:
            print(
                "  absorb    skipped: --from-measured replays a record, it does not measure the tree"
            )
        elif not set(record) >= set(old):
            print(
                "  absorb    REFUSED: this run measured %d scenario(s), the baseline holds %d -- "
                "absorbing would delete %s"
                % (len(record), len(old), ", ".join(sorted(set(old) - set(record))[:4]))
            )
        elif {k: v["files"] for k, v in record.items()} == {
            k: _files_of(v) for k, v in old.items()
        }:
            print(
                "  absorb    nothing to absorb -- the measured record already matches the baseline"
            )
        else:
            write_baseline(path, domain, record)
            print(
                "  absorb    baseline RE-RECORDED from this green run (%d gain(s), %d reshaped, "
                "%d new, %d stale entr(ies) cleared)"
                % (len(gains), len(res["reshaped"]), len(added), len(res["stale"]))
            )
        return 0

    write_baseline(path, domain, record)
    print("wrote %s (%d scenario(s))" % (path, len(record)))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument(
        "--suite",
        action="append",
        default=[],
        help="offline oracle suite name (routed to its exe by tools/data/selftest_roster.json); "
        "repeatable",
    )
    ap.add_argument("--cmd", help="arbitrary command line to measure instead of a suite")
    ap.add_argument(
        "--journal",
        help="measure a RIG SCENARIO: provision a tactical lane, replay this journal under the "
        "collector, and report which of our bodies the scenario actually entered",
    )
    ap.add_argument("--frames", type=int, default=0, help="--journal: frame budget (0 = derive)")
    ap.add_argument(
        "--steps",
        type=int,
        default=0,
        help="--baseline --domain sim*: override the registry's per-scenario step count. A deep run "
        "reaches states a short one cannot, at a cost that scales with it; the committed baseline stays "
        "whatever the registry says, so use this for a probe and re-record deliberately.",
    )
    ap.add_argument(
        "--milestones",
        default="",
        metavar="N,N,N",
        help="measure the DEPTH CURVE: run every registered scenario once per step budget in this "
        "comma-separated ladder and report rows executed per rung, with the steps each run actually "
        "reached. Answers 'does a deeper soak buy anything' with a number. Writes "
        "tmp/coverage_curve_<domain>.json and NEVER the committed baseline -- a curve is a measurement.",
    )
    ap.add_argument(
        "--speed",
        type=int,
        default=0,
        help="--baseline --domain sim*: [net] game_speed_pct for the soak. 1000 is the measured clean "
        "band for an 8-way AI match; 8000 stalls it at step 1475.",
    )
    ap.add_argument(
        "--timeout",
        type=int,
        default=0,
        help="per-run wall-clock seconds handed to the inner soak. Without it test_ui derives "
        "240 + steps/40, which truncates a long run under the collector -- and a truncated coverage run "
        "reports a quietly LOW number rather than failing.",
    )
    ap.add_argument(
        "--extra-ini",
        action="append",
        default=[],
        help="--journal: ini fragment(s) merged into the lane, e.g. a [promote] arm",
    )
    ap.add_argument(
        "--debug-dll",
        action="store_true",
        help="--journal: build an unoptimised mh.dll and deploy it into the lane, so line "
        "attribution is faithful (a Release 0%% can mean 'inlined away'). Slower to run; use it "
        "when you need the number to be trustworthy on its own.",
    )
    ap.add_argument(
        "--sources",
        # src/mh_dll, NOT src/mh_dll/mh, since fork F5O: the module tree became two
        # directories (mh/ + libmh/) and OpenCppCoverage takes ONE path here, so a filter
        # naming only mh/ would report 0%% for all 628 roster TUs instead of failing.
        default=os.path.join(REPO, "src", "mh_dll"),
        help="source subtree to attribute coverage to",
    )
    ap.add_argument(
        "--focus", action="append", default=[], help="print this file's figure explicitly"
    )
    ap.add_argument(
        "--lines",
        action="append",
        default=[],
        help="print the HIT and MISSED source lines for files matching this substring. A file-level "
        "percentage can be entirely install/registration code with the body at zero -- measured: "
        "tact_unit_stand_tick.cpp reads 15%% whose every hit is the shadow-install block.",
    )
    ap.add_argument(
        "--baseline",
        action="store_true",
        help="run EVERY registered tactical scenario under the collector and write the committed "
        "per-file record to tools/data/tact_coverage.json. Implies --debug-dll: a baseline built "
        "from an optimised build records inlining artefacts as real zeroes.",
    )
    ap.add_argument(
        "--check",
        action="store_true",
        help="--baseline: compare against the committed record instead of rewriting it. DROPS-ONLY "
        "(2026-09-11): FAILS when one of this domain's OWN files loses more covered lines than its "
        "own edit removed, when a row still in the census goes cold, or when one of its own files "
        "vanishes from the report with code still in the tree. Gains, new files, reshaped files, "
        "census-dropped rows and every foreign file's movement are reported, never failed on -- and "
        "a GREEN run re-records the baseline unless --no-absorb.",
    )
    ap.add_argument(
        "--no-absorb",
        action="store_true",
        help="--baseline --check: do NOT re-record the committed baseline when the check is GREEN. "
        "The default is to absorb, so the record tracks the tree; use this to read a green without "
        "touching the working tree.",
    )
    ap.add_argument(
        "--dump-measured",
        metavar="PATH",
        help="--baseline: also write the freshly MEASURED record to PATH (never the committed "
        "baseline). Replay it later with --from-measured to re-run the comparison offline.",
    )
    ap.add_argument(
        "--from-measured",
        metavar="PATH",
        help="--baseline --check: skip measurement and compare a record dumped earlier by "
        "--dump-measured. Lets a red be re-analysed, and the comparison rule be changed, without "
        "a second instrumented pass. Refused without --check.",
    )
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="run the drift comparison's own arms offline (no collector, no rig) and exit",
    )
    ap.add_argument(
        "--combined",
        action="store_true",
        help="union every committed tools/data/<domain>_coverage.json and report what the WHOLE "
        "scenario corpus executes, plus each scenario's UNIQUE contribution. Pure data -- no rig "
        "time and no build. Writes tools/data/combined_coverage.json.",
    )
    ap.add_argument(
        "--domain",
        default="tact",
        help="which registry --baseline walks: 'tact' (TACT_SCENARIOS journals, the default), "
        "'sim' (SIM_SCENARIOS all-AI soaks), or 'uirec' (the recorded CAMPAIGN session, ship/PROMOTED "
        "arm -- the only fixture reaching campaign init and the planet transition). Also selects the <domain>_liveness.json the per-row "
        "attribution reads and the tools/data/<domain>_coverage.json it writes.",
    )
    ap.add_argument(
        "--runs",
        type=int,
        default=3,
        help="--baseline (sim only): repeat each scenario N times and store the INTERSECTION of the "
        "rows that ran, so the gated set is the reproducible core. Measured 2026-09-05: two identical "
        "soak_saved runs covered 214 then 223 rows, the extra nine random-event driven -- gating on a "
        "single run's set would be flaky. Ignored for --check (one run) and for tact (journals replay "
        "reproducibly).",
    )
    ap.add_argument(
        "--rows",
        action="store_true",
        help="report PER-ROW execution for the measured report(s) instead of only the per-file table: "
        "how many verified rows of --domain actually ran, split by liveness route. This is the half a "
        "per-file figure cannot give -- 54%% of sim's rows share a TU with another row.",
    )
    ap.add_argument("--html", action="store_true", help="also write an HTML report")
    ap.add_argument(
        "--release",
        action="store_true",
        help="measure the OPTIMISED build anyway -- line attribution will be wrong",
    )
    args = ap.parse_args()

    if args.selftest:
        return selftest_compare()

    occ = find_occ()
    if not occ:
        print("FAIL: OpenCppCoverage not found. Install it:")
        print("      winget install --id OpenCppCoverage.OpenCppCoverage")
        print("      (or set OPENCPPCOVERAGE in tools/machine.local.json)")
        return 2

    os.makedirs(COV_DIR, exist_ok=True)
    if args.milestones:
        return run_milestones(args, occ)
    if getattr(args, "combined", False):
        return run_combined(args)
    if args.baseline:
        return run_baseline(args, occ)
    rig_cwd = None
    if args.journal:
        # THE RIG CASE, and it works despite the harness ending the run with TerminateProcess:
        # that still raises an EXIT_PROCESS debug event, so the collector writes its report
        # (measured -- 1.87 MB in 13 s on poz-stand-exit). The DLL under test is the RELEASE build
        # the lane deploys, so treat a 0% as "not entered OR inlined away" and corroborate before
        # acting; the mh project's Debug configuration does not currently build (stale include
        # paths), which is what a faithful rig figure would need.
        rig_cmd, rig_cwd = _rig_cmd(args)
        targets = [("rig_%s" % os.path.splitext(os.path.basename(args.journal))[0], rig_cmd)]
    elif args.cmd:
        targets = [("custom", args.cmd.split())]
    else:
        suites = args.suite or ["tacttest"]
        # PER SUITE, because the two exes answer to disjoint suite sets (F5I S4). A single `exe`
        # here would have silently measured `net_selftest.exe tacttest`, which exits 2.
        targets = []
        if args.release:
            print("*** --release: /O2 + WholeProgramOptimization inlines the small bodies away.")
            print("*** A 0% here means 'inlined', NOT 'never executed'. Do not act on it.")
        built = {}
        for s in suites:
            if args.release:
                exe = os.path.join(REPO, "src", "mh_dll", "Release", suite_build(s)[1])
            else:
                proj = suite_build(s)[0]
                if proj not in built:
                    built[proj] = build_debug(s)
                exe = built[proj]
                if not exe:
                    return 1
            targets.append((s, [exe, s]))

    rc = 0
    for name, cmd in targets:
        xml_path = os.path.join(COV_DIR, "%s.xml" % name)
        # ABSOLUTE. The rig case runs with cwd=<lane>, so a relative --sources resolves against
        # the lane and matches nothing -- which surfaced as "no report written" and reads exactly
        # like the TerminateProcess hazard it is not.
        sources = os.path.abspath(args.sources)
        occ_args = [
            occ,
            "--quiet",
            "--sources",
            sources,
            "--export_type",
            "cobertura:%s" % xml_path,
        ]
        if args.html:
            occ_args += ["--export_type", "html:%s" % os.path.join(COV_DIR, "%s_html" % name)]
        occ_args += ["--"] + cmd
        print("=" * 78)
        print("coverage: %s" % name)
        print("=" * 78)
        r = subprocess.run(occ_args, capture_output=True, text=True, cwd=rig_cwd)
        tail = [ln for ln in r.stdout.splitlines() if ln.strip()][-1:] or ["(no output)"]
        print("  run       %s" % tail[0][:100])
        if not os.path.isfile(xml_path):
            print("FAIL: no report written. Collector stderr:")
            for ln in (r.stderr or "").splitlines()[-6:]:
                print("      %s" % ln[:110])
            rc = 1
            continue
        summarise(xml_path, args.focus)
        if args.rows:
            report_rows(xml_path, args.domain)
        if args.lines:
            show_lines(xml_path, args.lines, os.path.abspath(args.sources))
        print("  report    %s" % xml_path)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
