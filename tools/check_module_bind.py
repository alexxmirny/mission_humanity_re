#!/usr/bin/env python3
"""check_module_bind.py -- fork F4A: DID A MISSING MODULE DEGRADE, OR DID IT KILL THE BOOT?

F4A's done_when asks for "a gate proves a missing module degrades rather than fails to boot". This
is that gate's log half. It reads ONE run directory's `mh_net.log` and answers two questions that a
green scenario cannot:

  1. WHAT HAPPENED TO THE BIND -- bound, absent, wrong module, ABI mismatch. A UI scenario walking
     to the menu proves the process survived; it says nothing about whether the module question was
     even asked, and a knob that silently stopped being read would leave the scenario just as green.
  2. DID THE BOOT CONTINUE PAST IT -- the arm window's END MARKER (`; [interlock] ... detour
     install(s)`) must appear AFTER the bind's outcome line. That ordering is the whole claim: the
     process did not merely start, it ran the entire arm with the module missing.

WHY IT IS A SEPARATE TOOL AND NOT A `[uitest]` PREDICATE. The script grammar's predicates read UI
STATE -- an active screen, a widget, a slot count. "A module refused to load and the boot carried
on" has no pixels. A capture can prove the menu rendered (and the registered scenario does exactly
that); only the log can prove WHY it was allowed to.

---- ABSENCE IS A FAILURE (the instrument-channel notes, and check_arm_order's own rule) ------------

A run that never reached the bind and a run that bound perfectly produce the same clean grep, so
every path here REFUSES rather than passes: no run directory, no `mh_net.log`, an empty log, no
`[modules]` line at all, an outcome line this tool does not recognise, or an end marker that is
missing or arrives BEFORE the outcome. None of those is a PASS.

---- MODES ---------------------------------------------------------------------------------------

  python tools/check_module_bind.py <run-dir>                  report the outcome, gate the boot
  python tools/check_module_bind.py <run-dir> --expect absent  ... and REQUIRE the absent-tolerant path
  python tools/check_module_bind.py <run-dir> --expect bound   ... and REQUIRE a working bind
  python tools/check_module_bind.py --selftest                 planted logs: every negative goes RED

  python tools/check_module_bind.py --dllmain-inert            F4A's OTHER clause, scanned off the
                                                               tree rather than a run: every DllMain
                                                               but mh.dll's orchestrator touches
                                                               nothing outside its own module
  python tools/check_module_bind.py --dllmain-inert --selftest planted satellites: every negative RED

  python tools/check_module_bind.py --net-surface              F4B: mh.dll reaches mh_net.dll ONLY
                                                               through the bound table, and the
                                                               table, the .def and the header agree
  python tools/check_module_bind.py --net-surface --selftest   planted drift: every negative RED

  python tools/check_module_bind.py --subset                   F4B: mh_net.dll's static imports are
                                                               a SUBSET of mh.dll's (dumpbin; needs
                                                               a Release build)

The three live recipes, all in docs/dll-split.md:

  ABSENT       python tools/test_ui.py module_absent     (the registered suite scenario -- the lane
               is built with `--omit-satellite mh_net.dll`, so the absence is REAL, not simulated)
  BOUND        any other lane: make_lane deploys mh_net.dll by default, so every normal run is the
               bound arm and `check_module_bind.py <lane> --expect bound` reads it
  NOT ATTEMPTED  a lane with `--net-extra module=none` (the no_net_boot scenario's shape)
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

LOG = "mh_net.log"

# The four outcomes module_bind.cpp can report, in the order it can report them. `key` is what
# --expect matches on; `ok_to_boot` is True for all of them, because every one of them is a path
# that CONTINUES -- which is the design under test, not an accident.
#
# EVERY PATTERN CAPTURES THE MODULE TAG, and since fork F4D that is load-bearing rather than tidy:
# a boot now writes TWO outcome lines, one per satellite (`mh_net` then `libmh`), and the old
# module-agnostic form read the second one as "two bind outcomes in one log", which classify()
# refuses. `--module` selects which satellite a run is being asked about; it defaults to mh_net so
# every committed invocation keeps its meaning.
OUTCOMES = [
    ("absent", re.compile(r"\[modules\]\s+\S+:\s+NOT BOUND\b")),
    # fork F4B. `[net] module=none` -- the run declined the load, so there is no file to look for
    # and no Win32 error to name. A SEPARATE outcome from `absent` on purpose: "the module is not
    # there" and "this run was told not to look" are different facts about a boot, and collapsing
    # them is the `[net] enable` conflation F3B spent an item undoing one level up.
    ("declined", re.compile(r"\[modules\]\s+\S+:\s+NOT ATTEMPTED\b")),
    ("wrong_module", re.compile(r"\[modules\]\s+\S+:\s+LOADED BUT REFUSED\b")),
    ("abi_mismatch", re.compile(r"\[modules\]\s+\S+:\s+ABI MISMATCH\b")),
    ("bound", re.compile(r"\[modules\]\s+\S+:\s+BOUND at\b")),
]

# Any `[modules]` line at all. Used to tell "the bind reported nothing recognisable" (a REFUSAL --
# the tool cannot make a statement) apart from "the knob was off" (also a refusal, but a different
# sentence, and the one an operator is far more likely to have caused).
ANY_MODULE_RE = re.compile(r"\[modules\]\s")

# MH_Seam_Init's last report -- the same marker check_arm_order uses to end the arm window. Its
# presence AFTER the outcome is the "the boot continued" proof.
END_RE = re.compile(r";\s*\[interlock\]\s+.*detour install\(s\)\s")

# The two facts that make a `bound` verdict non-vacuous: GetProcAddress succeeding is not evidence
# that a CALL across the boundary works, so the bind CALLS one export whose answer it already knows
# and prints the verdict. Deliberately MODULE-AGNOSTIC since F4B (the F4A spike printed its own
# arithmetic, `call-through 2+3=5`; mh_net.dll calls MH_Net_IsStarted, whose answer at
# DLL_PROCESS_ATTACH is necessarily 0) -- what this gate asserts is that a call was made and the
# module got the right answer, not which call it was. The ABI is still matched exactly, one line
# over, by the module itself.
BOUND_OK_RE = re.compile(r"init returned [0-9A-F]{8}, call-through ok\b")


class Refusal(Exception):
    """A run this tool cannot make a statement about. NEVER a pass."""


def read_log(run_dir):
    if not os.path.isdir(run_dir):
        raise Refusal("no such run directory: %s" % run_dir)
    path = os.path.join(run_dir, LOG)
    if not os.path.isfile(path):
        raise Refusal(
            "no %s in %s -- the DLL never got as far as opening its log, which is a dead boot, "
            "not a degraded one" % (LOG, run_dir)
        )
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()
    if not lines:
        raise Refusal("%s is empty" % path)
    return lines


MODULE_TAG_RE = re.compile(r"\[modules\]\s+(\S+):")


def classify(lines, module="mh_net"):
    """(outcome_key, outcome_lineno, outcome_text, end_lineno) for ONE satellite.
    Raises Refusal when it cannot tell."""
    hit = None
    for i, ln in enumerate(lines):
        tag = MODULE_TAG_RE.search(ln)
        if tag is None or tag.group(1) != module:
            continue
        for key, rx in OUTCOMES:
            if rx.search(ln):
                if hit is not None:
                    raise Refusal(
                        "two `%s` bind outcomes in one log (line %d and line %d) -- each satellite "
                        "binds at most once per process" % (module, hit[1] + 1, i + 1)
                    )
                hit = (key, i, ln)
    if hit is None:
        others = sorted(
            {
                m.group(1)
                for m in (MODULE_TAG_RE.search(ln) for ln in lines)
                if m is not None and m.group(1) != module
            }
        )
        if others:
            raise Refusal(
                "the log has `[modules]` outcome lines for %s but NONE for `%s`. A satellite whose "
                "bind reports nothing is one that was not bound at all, which is not a weaker pass "
                "-- it is a different boot from the one the caller asked about."
                % (", ".join(others), module)
            )
        if any(ANY_MODULE_RE.search(ln) for ln in lines):
            raise Refusal(
                "the log carries `[modules]` lines but none of them is an outcome this build "
                "knows -- module_bind.cpp and this checker have drifted apart"
            )
        raise Refusal(
            "no `[modules]` line at all. Since fork F4B the bind is UNCONDITIONAL -- every boot "
            "writes one outcome line per satellite -- so this means either the log is from a "
            "pre-F4B build or the bind stopped being called from DllMain, and neither is "
            "something this tool may pass. (It is also the row that matters most: a build that "
            "quietly stopped binding would leave an unchanged main-menu capture just as green.)"
        )

    end = None
    for i, ln in enumerate(lines):
        if END_RE.search(ln):
            end = i
    if end is None:
        raise Refusal(
            "the arm window has no end marker (`; [interlock] ... detour install(s)`): the boot "
            "did not finish arming, so 'it degraded gracefully' is not a statement this log "
            "supports"
        )
    return hit[0], hit[1], hit[2], end


def check(run_dir, expect, module="mh_net"):
    lines = read_log(run_dir)
    key, at, text, end = classify(lines, module)
    print("check_module_bind: %s [%s]" % (run_dir, module))
    print("  outcome  %-13s (log line %d)" % (key, at + 1))
    print("  %s" % text.strip())

    fails = []
    if end <= at:
        fails.append(
            "the arm's end marker is at line %d, BEFORE the bind outcome at line %d -- the bind "
            "did not happen inside the boot this log describes" % (end + 1, at + 1)
        )
    else:
        print("  boot     CONTINUED (arm end marker at line %d, after the outcome)" % (end + 1))

    if expect and expect != key:
        fails.append(
            "expected the `%s` outcome, got `%s`. A run that produced a different outcome from the "
            "one the caller named is not a weaker pass, it is a different experiment."
            % (expect, key)
        )
    # AGAINST THE OUTCOME LINE, not the whole log. It used to scan every line, which was correct
    # while a boot had exactly one satellite and became silently wrong at fork F4D: with two
    # `[modules]` lines, a broken call-through on THIS module passed on the other one's `ok`.
    # Caught by the selftest's two-module replay of its own existing cases.
    if key == "bound" and not BOUND_OK_RE.search(text):
        fails.append(
            "the module bound but the line does not read `init returned <abi>, call-through ok` -- "
            "resolving an export is not the same as being able to call it, so a bind that cannot "
            "show a correct answer from across the boundary is not a bind this gate may pass"
        )

    for f in fails:
        print("[FAIL] %s" % f)
    if fails:
        return 1
    print("check_module_bind: PASS")
    return 0


# ---- --libmh: THE SPINE-CROSSING ARM (fork F4D) ---------------------------------------------------
CROSSINGS_RE = re.compile(
    r";\s*\[libmh\]\s+crossings=(\d+)\s+absent-calls=(\d+)\b.*?configuration\s+\((\d)\)"
)


def check_libmh(run_dir, expect):
    """The bind verdict AND the crossing witness, together, because neither alone is the claim.

    THE MISS THIS CLOSES, stated because it is the reason the item exists. LIB-SPINE-API (13e7e4ec)
    proved the spine ABI live ONCE -- 86 routed rows, the full battery through the brokered entry --
    and then nothing in any gate ever asserted it again. After F4D a brokered lane that binds
    libmh.dll and never calls through it looks exactly like one that crossed four hundred times:
    the `[modules] libmh: BOUND` line is identical, the game boots, the menu renders, the capture
    matches. So the bind verdict is half a statement, and this is the other half.

    Two arms, and the absent one is not decoration: a lane with no libmh.dll must report crossings
    == 0 and absent-calls > 0. That is what makes the counters a measurement rather than a constant
    -- the same number appearing in both configurations would mean the instrument was not reading
    anything.
    """
    lines = read_log(run_dir)
    key, at, text, end = classify(lines, "libmh")
    print("check_module_bind --libmh: %s" % run_dir)
    print("  outcome  %-13s (log line %d)" % (key, at + 1))
    print("  %s" % text.strip())

    fails = []
    if end <= at:
        fails.append(
            "the arm's end marker is at line %d, BEFORE the libmh outcome at line %d -- the bind "
            "did not happen inside the boot this log describes" % (end + 1, at + 1)
        )
    if expect and expect != key:
        fails.append(
            "expected the `%s` outcome for libmh, got `%s`. A run that produced a different "
            "outcome from the one the caller named is not a weaker pass, it is a different "
            "experiment." % (expect, key)
        )

    hits = [(i, m) for i, m in ((i, CROSSINGS_RE.search(ln)) for i, ln in enumerate(lines)) if m]
    if not hits:
        raise Refusal(
            "no `; [libmh] crossings=...` line. It is emitted once from on_present, i.e. on the "
            "FIRST PRESENTED FRAME -- so a log without it is a run that never presented a frame, "
            "not a run that crossed zero times. Those are different facts and this tool will not "
            "report the second when it measured the first."
        )
    if len(hits) > 1:
        raise Refusal(
            "%d `; [libmh] crossings=` lines -- the report is emitted once per process and a "
            "second one means the guard stopped working" % len(hits)
        )
    i, m = hits[0]
    crossings, absent_calls, config = int(m.group(1)), int(m.group(2)), int(m.group(3))
    print(
        "  crossings %-6d absent-calls %-6d configuration (%d)  (log line %d)"
        % (crossings, absent_calls, config, i + 1)
    )
    if i <= end:
        fails.append(
            "the crossing report is at line %d, INSIDE the arm window (which ends at line %d). It "
            "is supposed to land after the whole arm has run, on the first present -- a report "
            "from mid-arm is a prefix of the crossings, not the arm's total." % (i + 1, end + 1)
        )

    if key == "bound":
        if config != 2:
            fails.append(
                "libmh BOUND but the report says configuration (%d). The two are derived from the "
                "same flag, so they cannot disagree unless something reset it mid-run." % config
            )
        if crossings == 0:
            fails.append(
                "libmh bound and the spine boundary was NEVER CROSSED (crossings=0). This lane "
                "carries the module and did not call it, which is the silent-fallback shape this "
                "arm exists to red on: every promotion installer, every observer registration and "
                "every state-registry read is a call across that boundary, so zero of them means "
                "the run was not the run it looked like."
            )
        if absent_calls != 0:
            fails.append(
                "libmh bound but %d call(s) took the ABSENT path. The table is adopted whole or "
                "not at all, so a mixed run means a slot was null after the bind reported success."
                % absent_calls
            )
    else:
        # `absent` is a SHIPPED configuration; `wrong_module` and `abi_mismatch` are not. They land
        # in the same degraded process -- same null table, same original bodies -- which is exactly
        # why they need saying out loud here: without this, a lane whose libmh.dll is the wrong file
        # passes this gate as long as nobody wrote --expect. Measured on a real boot (a lane with
        # mh_net.dll planted under the name libmh.dll: LOADED BUT REFUSED, 0 of 101 resolved,
        # crossings=0, 240k frames, and every other gate green).
        if key != "absent":
            fails.append(
                "libmh reported `%s`, which is a BROKEN DEPLOYMENT, not a configuration. `absent` "
                "means the file is not there and the game runs the original bodies on purpose; "
                "this means a libmh.dll IS there and mh.dll refused it. The process degrades to "
                "the same place either way, so nothing else in the gate can tell them apart." % key
            )
        if config != 1:
            fails.append("libmh did not bind but the report says configuration (%d)" % config)
        if crossings != 0:
            fails.append("libmh is not bound and yet %d call(s) crossed the boundary" % crossings)
        if absent_calls == 0:
            fails.append(
                "libmh is absent and NOTHING took the absent path (absent-calls=0). The counters "
                "would then read the same in both configurations, which means they are not "
                "measuring anything -- a vacuous green is the one result this arm must not give."
            )

    for f in fails:
        print("[FAIL] %s" % f)
    if fails:
        return 1
    print("check_module_bind --libmh: PASS")
    return 0


# ---- selftest ------------------------------------------------------------------------------------
#
# Same shape as check_fork_d8 / check_fork_f2_drop / check_arm_order: a check whose red has never
# been seen is not a check. Each case is a planted log; the ones that must go red are the point.

ARM = [
    "[10:00:00.000] ; [uitest] lane=91 -- single-instance mutex renamed",
    "[10:00:00.001] ; build=EN (EN-only DLL; probe ok)",
]
END = "[10:00:00.900] ; [interlock] 3 detour install(s) REFUSED -- whatever fix each carried"
ABSENT = (
    "[10:00:00.000] ; [modules] mh_net: NOT BOUND -- LoadLibrary(C:\\x\\mh_net.dll) failed, "
    "Win32 error 126 (the file is not there). Continuing WITHOUT the module: this is the "
    "absent-tolerant path, not a failure of the boot."
)
DECLINED = (
    "[10:00:00.000] ; [modules] mh_net: NOT ATTEMPTED -- `[net] module=none` in mh_net.ini, so "
    "mh.dll does not look for the module at all. Continuing WITHOUT a transport: this is the "
    "requested configuration, not a failure."
)
BOUND = (
    "[10:00:00.010] ; [modules] mh_net: BOUND at DllMain (under the loader lock) -- 23 exports "
    "resolved, init returned F4B00001, call-through ok"
)
BOUND_BAD = BOUND.replace("call-through ok", "call-through WRONG -- the boundary call is broken")

CASES = [
    # (name, log lines or None for "no log at all", expect, must_pass)
    ("absent + full arm, --expect absent", [ABSENT] + ARM + [END], "absent", True),
    ("absent + full arm, no --expect", [ABSENT] + ARM + [END], None, True),
    ("bound + full arm, --expect bound", ARM + [BOUND, END], "bound", True),
    ("declined + full arm, --expect declined", [DECLINED] + ARM + [END], "declined", True),
    # --- the reds ---
    ("BOOT DIED: outcome but no end marker", [ABSENT] + ARM, "absent", False),
    ("END MARKER BEFORE the outcome", ARM + [END, ABSENT], "absent", False),
    ("BOUND when absence was expected", ARM + [BOUND, END], "absent", False),
    ("ABSENT when a bind was expected", [ABSENT] + ARM + [END], "bound", False),
    # THE TWO NO-TRANSPORT OUTCOMES ARE NOT INTERCHANGEABLE, in both directions. A lane that
    # deleted mh_net.dll and one that set `[net] module=none` reach the same degraded UI, which is
    # exactly why the gate has to tell them apart: otherwise `module_absent` would pass on a run
    # that never looked for the file, and the missing-module claim would be untested again.
    ("DECLINED when real absence was expected", [DECLINED] + ARM + [END], "absent", False),
    ("ABSENT when a declined load was expected", [ABSENT] + ARM + [END], "declined", False),
    ("bound but the call-through is WRONG", ARM + [BOUND_BAD, END], "bound", False),
    ("no [modules] line at all (bind not called)", ARM + [END], "absent", False),
    ("empty log", [], "absent", False),
    ("no mh_net.log", None, "absent", False),
]

# ---- fork F4D: a boot now writes TWO outcome lines, and the second one has its own arm ------------
LIBMH_BOUND = (
    "[10:00:00.012] ; [modules] libmh: BOUND at DllMain (under the loader lock) -- 100 exports "
    "resolved, init returned F4D00001, call-through ok; module DLL_PROCESS_ATTACH was AFTER "
    "mh.dll's by 4211 us (attach_calls=1 attach_tid=5028, mh.dll tid=5028)"
)
LIBMH_ABSENT = (
    "[10:00:00.012] ; [modules] libmh: NOT BOUND -- LoadLibrary(C:\\x\\libmh.dll) failed, Win32 "
    "error 126 (the file is not there). This run is CONFIGURATION (1): mh.dll does not contain "
    "the spine, so the game runs the original binary's own bodies."
)
XING = (
    "[10:00:02.000] ; [libmh] crossings=%d absent-calls=%d -- the spine boundary was %s in this "
    "run (configuration (%d))"
)
XING_BOUND = XING % (412, 0, "ENTERED", 2)
XING_BOUND_ZERO = XING % (0, 0, "NOT ENTERED", 2)
XING_BOUND_MIXED = XING % (412, 3, "ENTERED", 2)
XING_ABSENT = XING % (0, 57, "NOT ENTERED", 1)
XING_ABSENT_VACUOUS = XING % (0, 0, "NOT ENTERED", 1)

LIBMH_REFUSED = (
    r"[10:00:00.012] ; [modules] libmh: LOADED BUT REFUSED -- C:\x\libmh.dll resolved 0 of 101 "
    "contract symbols (init=0 probe=0); missing: ?ai_say@ai@mh@@YAXPBDZZ. Continuing in "
    "configuration (1)."
)

LIBMH_CASES = [
    # (name, log lines, expect, must_pass)
    ("bound + crossings > 0", ARM + [BOUND, LIBMH_BOUND, END, XING_BOUND], "bound", True),
    ("absent + absent-calls > 0", [LIBMH_ABSENT] + ARM + [END, XING_ABSENT], "absent", True),
    # --- the reds, and the first is THE ONE THIS ARM EXISTS FOR ---
    # A lane that carries libmh.dll, binds it, and never calls through it. Every gate this project
    # had before F4D reads that as a clean run: the bind line is right, the boot completes, the
    # menu renders. This is the silent fallback the item's done_when names.
    (
        "SILENT FALLBACK: bound and never crossed",
        ARM + [BOUND, LIBMH_BOUND, END, XING_BOUND_ZERO],
        "bound",
        False,
    ),
    (
        "VACUOUS ABSENT: absent and nothing took the absent path",
        [LIBMH_ABSENT] + ARM + [END, XING_ABSENT_VACUOUS],
        "absent",
        False,
    ),
    (
        "MIXED: bound but some calls took the absent path",
        ARM + [BOUND, LIBMH_BOUND, END, XING_BOUND_MIXED],
        "bound",
        False,
    ),
    (
        "no crossing report at all (the run never presented a frame)",
        ARM + [BOUND, LIBMH_BOUND, END],
        "bound",
        False,
    ),
    (
        "the report lands INSIDE the arm window",
        ARM + [BOUND, LIBMH_BOUND, XING_BOUND, END],
        "bound",
        False,
    ),
    (
        "bound when configuration (1) was expected",
        ARM + [BOUND, LIBMH_BOUND, END, XING_BOUND],
        "absent",
        False,
    ),
    (
        "the mh_net outcome is there and libmh's is not",
        ARM + [BOUND, END, XING_BOUND],
        "bound",
        False,
    ),
    # A libmh.dll that IS there and was refused. It degrades to exactly where `absent` degrades,
    # so without this row the gate passes it whenever the caller wrote no --expect -- and a lane
    # with the wrong file in it is the shape a deployment bug actually takes.
    (
        "WRONG FILE: a libmh.dll that loaded and was refused, with no --expect",
        [LIBMH_REFUSED] + ARM + [END, XING_ABSENT],
        None,
        False,
    ),
    (
        "the counters and the configuration word disagree",
        ARM + [BOUND, LIBMH_BOUND, END, XING % (412, 0, "ENTERED", 1)],
        "bound",
        False,
    ),
]


def _run_cases(cases, fn, label):
    bad = 0
    for name, body, expect, must_pass in cases:
        with tempfile.TemporaryDirectory() as d:
            if body is not None:
                with open(os.path.join(d, LOG), "w", encoding="utf-8") as fh:
                    fh.write("\n".join(body))
            try:
                got = fn(d, expect) == 0
            except Refusal as e:
                print("%s REFUSED: %s" % (label, e))
                got = False
        verdict = "ok" if got == must_pass else "SELFTEST FAILED"
        if got != must_pass:
            bad += 1
        print(
            "  [%s] %-52s wanted %s, got %s\n"
            % (verdict, name, "PASS" if must_pass else "RED", "PASS" if got else "RED")
        )
    return bad


def selftest():
    # The mh_net cases run against a log that now ALSO carries libmh's outcome line, because that is
    # what a real boot writes and a per-module gate has to be proven on a two-module log rather than
    # on the one-module one it was written against.
    two_module = [
        (name, (body + [LIBMH_BOUND]) if body else body, expect, ok)
        for name, body, expect, ok in CASES
    ]
    bad = _run_cases(CASES, check, "check_module_bind")
    bad += _run_cases(two_module, check, "check_module_bind [+libmh line]")
    bad += _run_cases(LIBMH_CASES, check_libmh, "check_module_bind --libmh")
    n = len(CASES) * 2 + len(LIBMH_CASES)
    if bad:
        print("check_module_bind --selftest: %d of %d case(s) wrong" % (bad, n))
        return 1
    print("check_module_bind --selftest: %d case(s) ok" % n)
    return 0


# ---- --dllmain-inert: the SATELLITE DllMain rule, as a gate rather than a paragraph --------------
#
# F4A's other done_when clause is "every satellite DllMain proven inert". With one satellite that is
# provable by reading it; with four (mh_net, libmh, mh_harness, and whatever comes after) it is a
# rule somebody has to remember, and a remembered rule is the G106 hand-list shape. So it is derived
# and scanned: EVERY DllMain in the tree is found, exactly one is exempt BY NAME (mh.dll's, the
# orchestrator), and every other body may call nothing outside the allow-list below.
#
# WHY THIS EXACT ALLOW-LIST. A satellite's DLL_PROCESS_ATTACH runs under the loader lock, inside its
# loader's own DllMain, on the only thread the process has. DisableThreadLibraryCalls is loader
# bookkeeping and is what the msvfw32 shim does for the same reason; the other two are pure reads of
# the calling thread's own state, needed for the loader-order measurement. Anything else -- a file,
# a hook, a LoadLibrary, a thread, a call into another module -- is work, and work is what a
# satellite must not do before mh.dll's DllMain has orchestrated it (docs/dll-split.md).
#
# WHAT IT CANNOT SEE, stated rather than hidden: this reads the DllMain BODY. A satellite whose C++
# static constructors do work has already broken the rule before the body runs (_DllMainCRTStartup
# runs them first), and no textual scan of DllMain will show it. Keeping satellite statics to
# zero-initialised PODs is still a human rule; this gate covers the half that can be mechanised.
# A DEFINITION, not a mention: the parameter list must be followed by a brace. `\bDllMain\s*\(`
# alone matched the PROSE -- module_bind.cpp's own log text says "bound at DllMain (under the loader
# lock)" -- and the scan then reported four satellites and refused on a file that has no DllMain at
# all. Comments and string literals are stripped before this runs, which removes the same class of
# false hit a second way.
DLLMAIN_RE = re.compile(r"\bDllMain\s*\([^;{}]*\)\s*\{")
# The orchestrator. Named, not pattern-matched: there is exactly one, it is the whole point of the
# F4A ruling that there is exactly one, and a pattern would quietly grow a second.
ORCHESTRATOR = os.path.join("src", "mh_dll", "mh", "mh.c")
DLLMAIN_ALLOWED = {"DisableThreadLibraryCalls", "GetCurrentThreadId", "QueryPerformanceCounter"}
# C/C++ keywords that a `name(` regex would otherwise read as calls.
NOT_CALLS = {"if", "for", "while", "switch", "return", "sizeof", "DllMain", "APIENTRY", "WINAPI"}
CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
SRC_EXT = (".c", ".cpp")


def _strip_noise(text):
    """Remove // comments, /* */ comments and string/char literals -- all three can hold a `name(`."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    text = re.sub(r"'(?:\\.|[^'\\])*'", "''", text)
    return text


def dllmain_body(path):
    """The text between DllMain's braces, comments and literals removed. Refusal if unparseable."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = _strip_noise(fh.read())
    m = DLLMAIN_RE.search(text)
    if m is None:
        raise Refusal("%s no longer defines DllMain" % path)
    open_at = text.rindex("{", m.start(), m.end())
    depth, i = 0, open_at
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_at + 1 : i]
        i += 1
    raise Refusal("%s: DllMain's body never closes" % path)


def find_dllmains(root):
    hits = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in ("attic", "Debug", "Release", ".vs")]
        for fn in filenames:
            if not fn.endswith(SRC_EXT):
                continue
            p = os.path.join(dirpath, fn)
            with open(p, encoding="utf-8", errors="replace") as fh:
                if DLLMAIN_RE.search(_strip_noise(fh.read())):
                    hits.append(p)
    return sorted(hits)


def check_dllmain_inert(repo):
    src = os.path.join(repo, "src")
    if not os.path.isdir(src):
        raise Refusal("no src/ under %s" % repo)
    found = find_dllmains(src)
    orch = os.path.join(repo, ORCHESTRATOR)
    if orch not in found:
        raise Refusal(
            "%s does not define DllMain any more. That file is the SOLE ORCHESTRATOR this rule is "
            "written around; if it moved, this exemption has to move with it rather than the scan "
            "silently having no orchestrator to exempt." % ORCHESTRATOR
        )
    satellites = [p for p in found if p != orch]
    if not satellites:
        raise Refusal(
            "no satellite DllMain found at all. A zero-subject scan is not a pass -- either the "
            "tree really has no DLL besides mh.dll (in which case this row has nothing to gate and "
            "should say so) or the scan stopped finding them"
        )

    print("check_module_bind --dllmain-inert: %d satellite DllMain(s)" % len(satellites))
    fails = []
    for p in satellites:
        rel = os.path.relpath(p, repo).replace("\\", "/")
        body = dllmain_body(p)
        calls = sorted({c for c in CALL_RE.findall(body) if c not in NOT_CALLS})
        bad = [c for c in calls if c not in DLLMAIN_ALLOWED]
        print("  %-44s calls: %s" % (rel, ", ".join(calls) or "(none)"))
        if bad:
            fails.append(
                "%s: DllMain calls %s. A satellite's DLL_PROCESS_ATTACH may touch nothing outside "
                "its own module -- the permitted set is %s (docs/dll-split.md, and msvfw32's "
                "Rule 1). Move the work into an exported init that mh.dll's DllMain calls."
                % (rel, ", ".join(bad), ", ".join(sorted(DLLMAIN_ALLOWED)))
            )
    for f in fails:
        print("[FAIL] %s" % f)
    if fails:
        return 1
    print("check_module_bind --dllmain-inert: PASS")
    return 0


# The planted satellites the selftest scans. The first must pass; the rest are the reds, one per
# thing a well-meaning author would actually write into a satellite's DllMain.
INERT_CASES = [
    (
        "inert (the mh_net.dll shape)",
        "BOOL APIENTRY DllMain(HMODULE m, DWORD r, LPVOID x) {\n"
        "  if (r == DLL_PROCESS_ATTACH) {\n"
        "    DisableThreadLibraryCalls(m);\n"
        "    g_tid = GetCurrentThreadId();\n"
        "    QueryPerformanceCounter(&t);\n"
        "  }\n  return TRUE;\n}\n",
        True,
    ),
    (
        "LoadLibrary in a satellite DllMain",
        'BOOL APIENTRY DllMain(HMODULE m, DWORD r, LPVOID x) {\n'
        '  if (r == DLL_PROCESS_ATTACH) LoadLibraryA("other.dll");\n  return TRUE;\n}\n',
        False,
    ),
    (
        "it opens its own log",
        'BOOL APIENTRY DllMain(HMODULE m, DWORD r, LPVOID x) {\n'
        '  if (r == DLL_PROCESS_ATTACH) CreateFileA("sat.log", 0, 0, 0, 0, 0, 0);\n'
        "  return TRUE;\n}\n",
        False,
    ),
    (
        "it arms itself",
        "BOOL APIENTRY DllMain(HMODULE m, DWORD r, LPVOID x) {\n"
        "  if (r == DLL_PROCESS_ATTACH) MH_Sat_Install();\n  return TRUE;\n}\n",
        False,
    ),
    (
        "the forbidden call hides in a comment only",
        "BOOL APIENTRY DllMain(HMODULE m, DWORD r, LPVOID x) {\n"
        "  // never LoadLibraryA(here) -- see docs/dll-split.md\n"
        '  const char *s = "CreateFileA(x)";\n'
        "  if (r == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(m);\n  return TRUE;\n}\n",
        True,
    ),
]


def selftest_inert():
    """Plant a fake tree per case: the real orchestrator plus one satellite."""
    bad = 0
    for name, body, must_pass in INERT_CASES:
        with tempfile.TemporaryDirectory() as d:
            orch = os.path.join(d, ORCHESTRATOR)
            os.makedirs(os.path.dirname(orch))
            with open(orch, "w", encoding="utf-8") as fh:
                fh.write("BOOL APIENTRY DllMain(HANDLE h, DWORD r, LPVOID x) { MH_Seam_Init(); }\n")
            sat = os.path.join(d, "src", "mh_dll", "sat", "sat.cpp")
            os.makedirs(os.path.dirname(sat))
            with open(sat, "w", encoding="utf-8") as fh:
                fh.write(body)
            try:
                got = check_dllmain_inert(d) == 0
            except Refusal as e:
                print("check_module_bind REFUSED: %s" % e)
                got = False
        if got != must_pass:
            bad += 1
        print(
            "  [%s] %-42s wanted %s, got %s\n"
            % (
                "ok" if got == must_pass else "SELFTEST FAILED",
                name,
                "PASS" if must_pass else "RED",
                "PASS" if got else "RED",
            )
        )
    # ... and the two structural refusals: no orchestrator, and no satellite.
    for name, files in (
        (
            "no orchestrator (mh.c moved)",
            {"src/mh_dll/sat/sat.cpp": INERT_CASES[0][1]},  # CITATION-OK
        ),
        ("no satellite at all", {ORCHESTRATOR.replace("\\", "/"): "BOOL DllMain(){}\n"}),
    ):
        with tempfile.TemporaryDirectory() as d:
            for rel, body in files.items():
                p = os.path.join(d, *rel.split("/"))
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "w", encoding="utf-8") as fh:
                    fh.write(body)
            try:
                got = check_dllmain_inert(d) == 0
            except Refusal as e:
                print("check_module_bind REFUSED: %s" % e)
                got = False
        if got:
            bad += 1
        print(
            "  [%s] %-42s wanted RED, got %s\n"
            % ("ok" if not got else "SELFTEST FAILED", name, "PASS" if got else "RED")
        )
    if bad:
        print("check_module_bind --dllmain-inert --selftest: %d case(s) wrong" % bad)
        return 1
    print("check_module_bind --dllmain-inert --selftest: %d case(s) ok" % (len(INERT_CASES) + 2))
    return 0


# ---- --net-surface: mh.dll REACHES mh_net.dll ONLY THROUGH THE BOUND TABLE (fork F4B) ------------
#
# F4B's done_when says "the 18-site list is 0 ungated (gated or ruled per site, committed as a
# check)". This is that check, and the shape of it follows from the shape of the fix.
#
# The 18 were MH_Net_* call sites reached with no transport-present test in front (launch.cpp 12,
# ui_drive.cpp 1, gfx_overlay.cpp 1, harness.cpp 1; desync_watch's 3 were already gated). F4B did
# NOT add 18 guards. It made the SURFACE carry absence: mh.dll defines all 23 contract symbols as
# forwarding shims over the bound table (mh/seams/module_bind.cpp), each answering the value the
# real body answers when the transport is not started. So "ungated" stops being a property of a
# call site and becomes impossible -- there is no way to reach the module except through a shim
# that already knows.
#
# WHICH MEANS THE THING TO GATE IS NOT A COUNT OF GUARDS. A grep for `if (transport_present())`
# would now measure nothing (and would go green on a tree where the shims were deleted). What has
# to hold instead is four statements, and each one is a way the construction could silently break:
#
#   1. TABLE == .def. Every contract row is exported by the module, and every export is a contract
#      row (plus the two module-level entries). A symbol in one and not the other is a null pointer
#      at runtime or dead weight in the DLL -- the G106 hand-list failure, one level down.
#   2. HEADER subset TABLE. Every MH_Net_* declared in mh_net_export.h has a row. A function added
#      to the header but not the table gets no shim, and mh.dll fails to LINK -- which is a fine
#      outcome, but saying so here names the fix instead of leaving an unresolved external.
#   3. CALL SITES subset TABLE. Every MH_Net_*/MH_Key_* call in src/mh_dll/mh/ resolves to a row.
#   4. THE TWO-TU RULE. module_bind.cpp (the shims) is in mh.vcxproj and NOT in mh_nettest.vcxproj;
#      module_bind_compiled_in.cpp is the other way round. That is what keeps "whoever compiles
#      net_transport.cpp does not get the shims" structural rather than remembered.
#
# Plus a FLOOR, because a scan that finds nothing must not read as clean (G106): fewer than
# MIN_SYMBOLS rows means the parse broke, not that the surface shrank.
MIN_SYMBOLS = 20

NET_MODULE_H = os.path.join("src", "mh_dll", "mh_common", "include", "mh_net_module.h")
NET_EXPORT_H = os.path.join("src", "mh_dll", "mh_common", "include", "mh_net_export.h")
NET_DEF = os.path.join("src", "mh_dll", "mh_net", "mh_net.def")
MH_VCXPROJ = os.path.join("src", "mh_dll", "mh", "mh.vcxproj")
NETTEST_VCXPROJ = os.path.join("src", "mh_dll", "mh_nettest", "mh_nettest.vcxproj")
# fork F5I S2: the second offline exe. It compiles the same 628 roster TUs as mh_nettest
# but in the STANDALONE arm, and it must carry NEITHER module_bind TU -- see the check.
LIBMH_TEST_VCXPROJ = os.path.join("src", "mh_dll", "libmh_test", "libmh_test.vcxproj")
MH_SEAMS_DIR = os.path.join("src", "mh_dll", "mh")

# The two module-level entries are exported but are NOT transport rows: mh.dll calls them directly
# from the bind, never through the table, so they are absent from MH_NET_MODULE_SYMBOLS by design.
MODULE_ENTRIES = {"MH_NetModule_Init", "MH_NetModule_Probe"}

# `X(ret, NAME, (params), (args), {absent})` -- the row name is the second macro argument.
ROW_RE = re.compile(r"^\s*X\(\s*[A-Za-z_][\w ]*\s*,\s*([A-Za-z_]\w*)\s*,", re.M)
DEF_EXPORT_RE = re.compile(r"^\s{4,}([A-Za-z_]\w*)\s*$", re.M)
DECL_RE = re.compile(r"^\s*(?:int|void)\s+(MH_Net_\w+|MH_Key_\w+)\s*\(", re.M)
CALL_RE_NET = re.compile(r"\b(MH_Net_\w+|MH_Key_\w+)\s*\(")
# Names that are NOT transport calls: the module-level entries, the bind's own predicate, and the
# transport's private CSPRNG entry (declared in mh_net_key.h, called only inside the module).
CALL_EXEMPT = MODULE_ENTRIES | {"MH_NetModule_IsBound", "MH_Key_Random"}
# ...plus anything mh.dll DEFINES for itself under that prefix. `MH_Net_Arm` is the live example:
# it is the net arm (file-static in net_seams.cpp), not a transport export, and the name pattern
# cannot tell them apart. Derived rather than listed, so mh.dll may name its own functions without
# this gate acquiring an exemption list somebody has to maintain -- the G106 shape it is here to
# prevent. A definition, not a mention: the parameter list must be followed by a brace.
LOCAL_DEF_RE = re.compile(r"\b(MH_Net_\w+|MH_Key_\w+)\s*\([^;{}]*\)\s*\{")


def _read(repo, rel):
    p = os.path.join(repo, rel)
    if not os.path.isfile(p):
        raise Refusal("missing %s -- this gate cannot make a statement without it" % rel)
    with open(p, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def check_net_surface(repo):
    table = ROW_RE.findall(_read(repo, NET_MODULE_H))
    if len(table) < MIN_SYMBOLS:
        raise Refusal(
            "MH_NET_MODULE_SYMBOLS parsed as %d row(s), under the floor of %d. A surface that small "
            "is a broken parse, not a smaller contract -- and a zero-row scan would pass every "
            "check below vacuously" % (len(table), MIN_SYMBOLS)
        )
    dups = sorted({n for n in table if table.count(n) > 1})
    tableset = set(table)

    exports = set(DEF_EXPORT_RE.findall(_read(repo, NET_DEF)))
    declared = set(DECL_RE.findall(_read(repo, NET_EXPORT_H)))

    sources = []
    for dirpath, dirnames, filenames in os.walk(os.path.join(repo, MH_SEAMS_DIR)):
        dirnames[:] = [d for d in dirnames if d not in ("Debug", "Release", ".vs", "attic")]
        for fn in sorted(filenames):
            if not fn.endswith((".c", ".cpp", ".h")):
                continue
            p = os.path.join(dirpath, fn)
            rel = os.path.relpath(p, repo).replace("\\", "/")
            if rel.endswith("mh/seams/module_bind.cpp"):
                continue  # the shims' own definitions ARE the surface
            sources.append((rel, _strip_noise(open(p, encoding="utf-8", errors="replace").read())))

    exempt = set(CALL_EXEMPT)
    for _rel, text in sources:
        exempt.update(LOCAL_DEF_RE.findall(text))
    calls = {}
    for rel, text in sources:
        for m in CALL_RE_NET.finditer(text):
            if m.group(1) not in exempt:
                calls.setdefault(m.group(1), []).append(rel)

    mh_proj = _read(repo, MH_VCXPROJ)
    test_proj = _read(repo, NETTEST_VCXPROJ)

    print("check_module_bind --net-surface: %d contract row(s)" % len(table))
    print("  .def exports      %d" % len(exports))
    print("  header decls      %d" % len(declared))
    print("  call sites        %d distinct name(s) across mh/" % len(calls))

    fails = []
    if dups:
        fails.append("MH_NET_MODULE_SYMBOLS lists %s twice" % ", ".join(dups))
    missing_export = sorted(tableset - exports)
    if missing_export:
        fails.append(
            "in the symbol table but NOT exported by mh_net.def: %s. mh.dll would GetProcAddress "
            "them, get null, and refuse the whole module at boot." % ", ".join(missing_export)
        )
    extra_export = sorted(exports - tableset - MODULE_ENTRIES)
    if extra_export:
        fails.append(
            "exported by mh_net.def but NOT in the symbol table: %s. Nothing in mh.dll can reach "
            "them -- either add a row (with its absent value) or stop exporting them."
            % ", ".join(extra_export)
        )
    missing_entry = sorted(MODULE_ENTRIES - exports)
    if missing_entry:
        fails.append(
            "mh_net.def does not export %s -- the bind calls these two directly and refuses the "
            "module without them" % ", ".join(missing_entry)
        )
    undeclared = sorted(declared - tableset)
    if undeclared:
        fails.append(
            "declared in mh_net_export.h with no table row: %s. It would have no forwarding shim, "
            "so mh.dll cannot link against it and no absent value is defined for it."
            % ", ".join(undeclared)
        )
    unbound_calls = sorted(set(calls) - tableset)
    if unbound_calls:
        fails.append(
            "called from mh/ but not in the bound table: %s (e.g. %s). EVERY call into the module "
            "must go through the bound-or-refused surface; this one would be an unresolved external "
            "or, worse, a second definition."
            % (", ".join(unbound_calls), calls[unbound_calls[0]][0])
        )
    # 4. the two-TU rule, in both directions.
    # The trailing `"` on the `avoid` needles is load-bearing: `module_bind.cpp` is a prefix of
    # `module_bind_compiled_in.cpp` only in the other direction, but matching the closing quote of
    # the Include attribute is what makes each needle name exactly one file.
    # The compiled-in needle carries NO directory since fork F5O: the file moved out of mh\seams\
    # into mh_nettest\, the one project that compiles it, so mh_nettest spells it bare and any
    # other project would have to spell a path ending in the same name.
    for proj_name, proj, want, avoid in (
        (
            "mh.vcxproj",
            mh_proj,
            'seams\\module_bind.cpp"',
            'module_bind_compiled_in.cpp"',
        ),
        (
            "mh_nettest.vcxproj",
            test_proj,
            'module_bind_compiled_in.cpp"',
            'seams\\module_bind.cpp"',
        ),
    ):
        if want not in proj:
            fails.append("%s does not compile %s" % (proj_name, want.rstrip('"')))
        if avoid in proj:
            fails.append(
                "%s compiles %s -- the shims and the real transport bodies must never be in one "
                "image (23 duplicate symbols), and that rule is kept by the project files, not by "
                "an #ifdef" % (proj_name, avoid.rstrip('"'))
            )
    # THE THIRD PROJECT CARRIES NEITHER (fork F5I S2). libmh_test.vcxproj compiles the roster too,
    # so it is exactly where the two-TU rule could be broken next, and the failure would be the same
    # 23 duplicate symbols -- or worse, a silent second answer if only one of the pair came across.
    # A separate check rather than a third row of the loop above because its want-set is EMPTY:
    # "this project compiles neither" has no positive half, and inventing a `want` for it would make
    # the check about the wrong file. _read() refuses a missing project, so the arm cannot silently
    # have nothing to read.
    libmh_test_proj = _read(repo, LIBMH_TEST_VCXPROJ)
    for needle in ('module_bind_compiled_in.cpp"', 'seams\\module_bind.cpp"'):
        if needle in libmh_test_proj:
            fails.append(
                "libmh_test.vcxproj compiles %s -- the shims and the real transport bodies must "
                "never be in one image, and libmh_test is a THIRD image compiling the roster. It "
                "needs neither TU: it links no transport at all." % needle.rstrip('"')
            )

    if "mh_net\\net_transport.cpp" in mh_proj:
        fails.append(
            "mh.vcxproj compiles net_transport.cpp. The transport lives in mh_net.dll since F4B; "
            "compiling it into mh.dll puts the real bodies next to their own forwarding shims."
        )

    for f in fails:
        print("[FAIL] %s" % f)
    if fails:
        return 1
    print("check_module_bind --net-surface: PASS")
    return 0


# The planted trees. The first must pass; the rest are each a way the construction breaks silently.
def _plant_surface(
    d, rows, exports, decls, calls_extra="", mh_extra="", test_extra="", libmh_test_extra=""
):
    def w(rel, text):
        p = os.path.join(d, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(text)

    body = "#define MH_NET_MODULE_SYMBOLS(X) \\\n"
    for r in rows:
        body += "    X(int, %s, (void), (), { return 0; }) \\\n" % r
    body += "    /* end */\n"
    w(NET_MODULE_H.replace("\\", "/"), body)
    w(
        NET_DEF.replace("\\", "/"),
        "LIBRARY mh_net\nEXPORTS\n" + "".join("    %s\n" % e for e in exports),
    )
    w(NET_EXPORT_H.replace("\\", "/"), "".join("int %s(void);\n" % d2 for d2 in decls))
    w(
        "src/mh_dll/mh/seams/caller.cpp",  # CITATION-OK
        "void f(){ %s(); %s }\n" % (rows[0], calls_extra),
    )
    w(
        MH_VCXPROJ.replace("\\", "/"),
        '<Project><ClCompile Include="seams\\module_bind.cpp" />%s</Project>\n' % mh_extra,
    )
    w(
        NETTEST_VCXPROJ.replace("\\", "/"),
        '<Project><ClCompile Include="module_bind_compiled_in.cpp" />%s</Project>\n' % test_extra,
    )
    # The THIRD project's shipped shape is EMPTY of both module_bind TUs (fork F5I S2), so the
    # planted-positive has to spell that emptiness rather than leaving the file out -- _read()
    # refuses a missing project, which is the point of the arm.
    w(
        LIBMH_TEST_VCXPROJ.replace("\\", "/"),
        '<Project><ClCompile Include="libmh_selftest.cpp" />%s</Project>\n' % libmh_test_extra,
    )


ROWS = ["MH_Net_R%02d" % i for i in range(MIN_SYMBOLS + 3)]

SURFACE_CASES = [
    ("the shipped shape", dict(rows=ROWS, exports=ROWS + sorted(MODULE_ENTRIES), decls=ROWS), True),
    (
        "a row nobody exports",
        dict(rows=ROWS, exports=ROWS[:-1] + sorted(MODULE_ENTRIES), decls=ROWS),
        False,
    ),
    (
        "the third project compiles the shims",
        dict(
            rows=ROWS,
            exports=ROWS + sorted(MODULE_ENTRIES),
            decls=ROWS,
            libmh_test_extra='<ClCompile Include="module_bind_compiled_in.cpp" />',
        ),
        False,
    ),
    (
        "the third project compiles the real bodies",
        dict(
            rows=ROWS,
            exports=ROWS + sorted(MODULE_ENTRIES),
            decls=ROWS,
            libmh_test_extra='<ClCompile Include="..\\mh\\seams\\module_bind.cpp" />',
        ),
        False,
    ),
    (
        "an export with no row",
        dict(rows=ROWS[:-1], exports=ROWS + sorted(MODULE_ENTRIES), decls=ROWS[:-1]),
        False,
    ),
    (
        "MH_NetModule_Init not exported",
        dict(rows=ROWS, exports=ROWS + ["MH_NetModule_Probe"], decls=ROWS),
        False,
    ),
    (
        "declared in the header, no row",
        dict(rows=ROWS, exports=ROWS + sorted(MODULE_ENTRIES), decls=ROWS + ["MH_Net_Ghost"]),
        False,
    ),
    (
        "a call site nothing binds",
        dict(
            rows=ROWS,
            exports=ROWS + sorted(MODULE_ENTRIES),
            decls=ROWS,
            calls_extra="MH_Net_Stray();",
        ),
        False,
    ),
    (
        "the selftest gets the shims too",
        dict(
            rows=ROWS,
            exports=ROWS + sorted(MODULE_ENTRIES),
            decls=ROWS,
            test_extra='<ClCompile Include="seams\\module_bind.cpp" />',
        ),
        False,
    ),
    (
        "mh.dll compiles the transport again",
        dict(
            rows=ROWS,
            exports=ROWS + sorted(MODULE_ENTRIES),
            decls=ROWS,
            mh_extra='<ClCompile Include="..\\mh_net\\net_transport.cpp" />',
        ),
        False,
    ),
    (
        "a truncated table (the floor)",
        dict(rows=ROWS[:3], exports=ROWS[:3] + sorted(MODULE_ENTRIES), decls=ROWS[:3]),
        False,
    ),
]


def selftest_surface():
    bad = 0
    for name, kw, must_pass in SURFACE_CASES:
        with tempfile.TemporaryDirectory() as d:
            _plant_surface(d, **kw)
            try:
                got = check_net_surface(d) == 0
            except Refusal as e:
                print("check_module_bind REFUSED: %s" % e)
                got = False
        if got != must_pass:
            bad += 1
        print(
            "  [%s] %-42s wanted %s, got %s\n"
            % (
                "ok" if got == must_pass else "SELFTEST FAILED",
                name,
                "PASS" if must_pass else "RED",
                "PASS" if got else "RED",
            )
        )
    if bad:
        print("check_module_bind --net-surface --selftest: %d case(s) wrong" % bad)
        return 1
    print("check_module_bind --net-surface --selftest: %d case(s) ok" % len(SURFACE_CASES))
    return 0


# ---- --subset: THE IMPORT-TABLE HALF OF THE SUBSET RULE (fork F4A's rule, F4B's first subject) ---
#
# docs/dll-split.md: "A satellite is safe to load from mh.dll's DllMain when (a) its own DllMain is
# inert, and (b) its static imports are a SUBSET of mh.dll's own." --dllmain-inert is (a). This is
# (b), and it needs the BINARIES, so it is not a lint row -- lint has no build (the same division
# check_net_lockstep_refs draws between --src-only and its OBJ mode). run_gate.py runs it straight
# after the Release build, where fresh binaries are guaranteed.
#
# WHY IT IS NOT OPTIONAL FOR F4B SPECIFICALLY. mh.dll imported WS2_32 for exactly one reason: the
# transport was compiled into it. Moving the transport out takes WS2_32 with it and breaks the rule
# on the very first satellite -- so module_bind.cpp keeps a deliberate one-instruction htons()
# anchor, and this gate is what stops that anchor being deleted as dead code. (Measured: mh.dll's
# WS2_32 import is a single ordinal, and it is the anchor's.)
SUBSET_DLL_RE = re.compile(r"^\s{4}([A-Za-z0-9_.\-]+\.dll)\s*$", re.M | re.I)


def find_dumpbin(explicit=None):
    """Same search as check_net_lockstep_refs -- kept local rather than imported so this tool has no
    dependency on that one."""
    if explicit:
        return explicit if os.path.isfile(explicit) else None
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        import machine_config as machine

        roots = [machine.VS_INSTALL_ROOT]
    except Exception:
        roots = []
    roots += [
        r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools",
        r"C:\Program Files\Microsoft Visual Studio\2022\Community",
    ]
    for root in roots:
        pat = os.path.join(root, "VC", "Tools", "MSVC", "*", "bin", "Host*", "*", "dumpbin.exe")
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[-1]
    return None


def imported_dlls(dumpbin, pe):
    out = subprocess.run([dumpbin, "-imports", pe], capture_output=True, text=True)
    if out.returncode != 0:
        raise Refusal("dumpbin failed on %s: %s" % (pe, out.stdout[-300:]))
    names = {n.upper() for n in SUBSET_DLL_RE.findall(out.stdout)}
    if not names:
        raise Refusal(
            "dumpbin reported NO imports for %s. Every PE here imports at least KERNEL32, so this "
            "is a parse failure, and a parse failure that reads as an empty set would make the "
            "subset test pass no matter what the module imports" % pe
        )
    return names


# The satellites the rule covers, and their loader. One row per satellite so F4D/F4E add a line.
# One row per satellite mh.dll loads from DLL_PROCESS_ATTACH. libmh.dll joined at fork F4D and
# docs/dll-split.md flagged it as the one to WATCH -- "that one links a C++ runtime" -- so it is
# worth recording what the measurement actually said: with the Release build pinning the STATIC CRT
# it imports KERNEL32 and USER32 and nothing else, i.e. it holds with room to spare. A dynamic CRT
# would put VCRUNTIME140 and MSVCP140 in that table and this row would go red, which is exactly the
# job. (mh.dll's own WS2_32 row is kept alive by module_bind.cpp's htons anchor; see that file.)
SUBSET_PAIRS = [("mh.dll", "mh_net.dll"), ("mh.dll", "libmh.dll"), ("mh.dll", "mh_harness.dll")]


def check_subset(repo, build_dir=None, dumpbin=None):
    build_dir = build_dir or os.path.join(repo, "src", "mh_dll", "Release")
    db = find_dumpbin(dumpbin)
    if db is None:
        raise Refusal("no dumpbin.exe found (machine_config VS_INSTALL_ROOT)")
    fails = []
    for loader, satellite in SUBSET_PAIRS:
        lp = os.path.join(build_dir, loader)
        sp = os.path.join(build_dir, satellite)
        for p in (lp, sp):
            if not os.path.isfile(p):
                raise Refusal(
                    "%s is not built. This gate reads IMPORT TABLES, so an unbuilt tree is a run it "
                    "cannot make a statement about -- build Release first" % p
                )
        loader_imports = imported_dlls(db, lp)
        sat_imports = imported_dlls(db, sp)
        extra = sorted(sat_imports - loader_imports)
        print("check_module_bind --subset: %s <- %s" % (loader, satellite))
        print("  %-12s %s" % (loader, " ".join(sorted(loader_imports))))
        print("  %-12s %s" % (satellite, " ".join(sorted(sat_imports))))
        if extra:
            fails.append(
                "%s imports %s, which %s does not. Loading it from %s's DLL_PROCESS_ATTACH would "
                "make the loader initialise %s under a lock we hold -- the open-ended-dependency "
                "hazard the subset rule exists for (docs/dll-split.md). Either add the import to "
                "%s too, so the loader initialises it BEFORE us, or move that satellite to "
                "mechanism B."
                % (satellite, ", ".join(extra), loader, loader, " and ".join(extra), loader)
            )
    for f in fails:
        print("[FAIL] %s" % f)
    if fails:
        return 1
    print("check_module_bind --subset: PASS")
    return 0


def newest_run(lane_dir):
    """The lane's most recent run folder, or None. Convenience for a caller holding a lane path."""
    runs = sorted(glob.glob(os.path.join(lane_dir, "logs", "*", LOG)), key=os.path.getmtime)
    return os.path.dirname(runs[-1]) if runs else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "run_dir", nargs="?", help="a run folder (…/logs/<stamp>_<role>) or a LANE folder"
    )
    ap.add_argument(
        "--expect",
        choices=[k for k, _ in OUTCOMES],
        help="require this outcome; without it any recognised outcome passes as long as the boot "
        "continued past it",
    )
    ap.add_argument(
        "--module",
        default="mh_net",
        help="which satellite's outcome line to read (a boot writes one per satellite since fork "
        "F4D). Default mh_net, so every committed invocation keeps its meaning.",
    )
    ap.add_argument(
        "--libmh",
        action="store_true",
        help="fork F4D's STANDING ARM: read libmh's bind outcome AND the `; [libmh] crossings=` "
        "report, and require them to agree -- a brokered lane that bound the spine and never "
        "crossed the boundary REDS instead of reading green. Use with --expect bound/absent.",
    )
    ap.add_argument(
        "--dllmain-inert",
        action="store_true",
        help="scan the tree instead of a run: every DllMain except mh.dll's own orchestrator must "
        "touch nothing outside its own module (F4A's satellite rule, as a gate)",
    )
    ap.add_argument(
        "--net-surface",
        action="store_true",
        help="fork F4B: mh.dll reaches mh_net.dll ONLY through the bound table, and the table, "
        "mh_net.def and mh_net_export.h agree (source only -- no build needed)",
    )
    ap.add_argument(
        "--subset",
        action="store_true",
        help="fork F4A's subset rule, import-table half: mh_net.dll's static imports must be a "
        "SUBSET of mh.dll's. Reads the built Release PEs with dumpbin.",
    )
    ap.add_argument("--build-dir", help="--subset: where the PEs are (default src/mh_dll/Release)")
    ap.add_argument("--dumpbin", help="--subset: an explicit dumpbin.exe")
    ap.add_argument(
        "--selftest", action="store_true", help="planted inputs: every negative goes RED"
    )
    args = ap.parse_args()

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if args.net_surface:
        try:
            return selftest_surface() if args.selftest else check_net_surface(repo)
        except Refusal as e:
            print("check_module_bind REFUSED: %s" % e)
            return 2
    if args.subset:
        try:
            return check_subset(repo, args.build_dir, args.dumpbin)
        except Refusal as e:
            print("check_module_bind REFUSED: %s" % e)
            return 2
    if args.dllmain_inert:
        try:
            return selftest_inert() if args.selftest else check_dllmain_inert(repo)
        except Refusal as e:
            print("check_module_bind REFUSED: %s" % e)
            return 2
    if args.selftest:
        return selftest()
    if not args.run_dir:
        ap.error("a run directory is required (or --selftest)")
    # A LANE folder is accepted and resolved to its newest run: every caller that has a path has the
    # lane, and making them each write the same glob is how two of them end up disagreeing about
    # which run they meant.
    run_dir = args.run_dir
    if not os.path.isfile(os.path.join(run_dir, LOG)):
        found = newest_run(run_dir)
        if found:
            run_dir = found
    try:
        if args.libmh:
            return check_libmh(run_dir, args.expect)
        return check(run_dir, args.expect, args.module)
    except Refusal as e:
        print("check_module_bind REFUSED: %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
