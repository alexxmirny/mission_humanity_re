#!/usr/bin/env python3
"""check_fork_f2_drop.py -- fork F2's drop gate: the per-row configuration vocabulary stays deleted.

F2 replaced a per-mechanism configuration surface with ONE selector. What went, across F2D/F2F/F2E:

  the differential-oracle vocabulary   MH_SHADOW_REPLACE sites, shadow_arm blocks, mh::shadow::,
                                       the `[shadow]` section, the shadowtest suite       (F2D)
  the deferred-effect machinery        mh::effects::, mh_effects.gen.h, effecttest, and the
                                       net_session read-back probe + its checker           (F2F)
  the per-row promotion surface        23 SHIP_PROMOTE_* constants, SHIP_REBIND_DEFAULT, and the
                                       57 ini reads of `[promote]` / `[promote_skip]` /
                                       `[state_handler_skip]` / `[rebind]`                 (F2E)
  the tactical install surface         18 MH_EXPORT_REPLACE promotions + tact_promote.{h,cpp}
                                       (tactical mode is the game's own in every hosted
                                       configuration this fork ships)                      (F2E)

WHAT REPLACED THEM, and therefore what this gate is really pinning: `[config] mode` in
mh/config/config.h, read once per process, answering `original` or `brokered` (or compiled to
`standalone`). A run whose ini still carries one of the retired sections is REFUSED by name before
anything installs -- so a survivor here is not a dormant knob, it is a file that kills lanes.

---- WHAT THIS GATE FORBIDS IS THE MECHANISM, NOT THE WORD, AND THAT IS A DELIBERATE NARROWING ----

The plain reading of "zero survivors of `[promote]`" would also delete 170 arm-log lines that PREFIX
themselves `; [promote] <domain>: ...`. Those lines are the evidence channel every one of this
item's own proofs diffs against, and their prefix is the channel's NAME, not a config key. Renaming
them would churn the one artifact the change is measured by, for no information. So:

  * the INSTALLER (a `GetPrivateProfile*("promote", ...)` read)     -- forbidden, assertion 2
  * the SECTION HEADER in any committed .ini                        -- forbidden, assertion 3
  * a tool WRITING such a section (`"[promote]\\n"` in a .py)        -- forbidden, assertion 4
  * a log line that says `; [promote] ...`                          -- ALLOWED, and expected

The same split applies to `[rebind]`: `mh::rebind::armed()` and its bitmap SURVIVE (ruling Q2 -- the
BIND survives, and the bitmap survives with it because harness.cpp still clears the rows an
instrument owns the entry of), while the section that used to fill it is gone.

---- THE ALLOWED SURVIVORS, BY NAME ----

  MH_LIBMH_BIND / MH_REBIND_TARGET_ / MH_LIBMH_BUILD   the binder itself (Q2)
  mh::rebind::{armed,arm_from_config,arm_none,set_armed,report_arming,trap_hit}
                                                       the arm bitmap and the entry-ownership yield
  state/rebind_arming.cpp, rebind_{targets,verify,traps}.gen.*, tools/data/libmh_rebind*.json
  MF_MEASURED                                          the write-census flag, renamed from MF_SHADOW
  `; [promote] ...` / `; [rebind] ...` log prefixes     the arm-log evidence channel
  tools/check_fork_d8.py                               it is BUILT from the effects vocabulary

---- THE BROKEN-GREP PROBLEM ----

Same as check_fork_d8's, and solved the same way: a zero-survivor scan has no anchor on the real
tree, because a regex that matches nothing and a regex that is broken produce the identical clean
result. So every assertion is re-fired in --selftest against a planted violation in a temp tree,
through this same walker, plus a walker-liveness arm that REFUSES a scan which visited nothing.

usage:
  python tools/check_fork_f2_drop.py            # the assertions (exit 1 on violation)
  python tools/check_fork_f2_drop.py --selftest # planted-violation reds + walker liveness
"""

import os
import tempfile
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src", "mh_dll")
TOOLS = os.path.join(REPO, "tools")

# F2G added three (plan D12): `pacing` (fps_cap moved into [video], beside the no_present it
# already needed), `test` (lane moved into [uitest]) and `probe` (the LT1C cell-grid diagnostic
# went away with its stub). They retire for a different reason from F2E's four -- those described
# a per-key control surface that no longer exists, these simply MOVED -- but the gate's question
# is identical either way: can the mechanism come back? Note the alternation is matched against a
# LITERAL `[name]` or a quoted section argument, so `[uitest]` does not match `test` and
# `net_session_probe` does not match `probe`.
RETIRED_SECTIONS = "promote|promote_skip|state_handler_skip|rebind|shadow|effects|pacing|test|probe"

# 1. The per-mechanism ship constants. Bare identifiers: nothing else in the tree spells them.
SHIP_RE = re.compile(r"\bSHIP_PROMOTE_\w*|\bSHIP_REBIND_DEFAULT\b")

# 2. THE INSTALLER. A read of a retired section is the mechanism itself -- this is the assertion that
#    would go red if any of the 57 collapsed call sites came back.
PROFILE_RE = re.compile(r'GetPrivateProfile\w+\(\s*"(?:%s)"' % RETIRED_SECTIONS)

# 3. A retired SECTION HEADER in a committed ini. Anchored at line start, which is what distinguishes
#    it from the `; [promote] ...` log prefix an ini's own banner may legitimately quote.
INI_SECTION_RE = re.compile(r"^\s*\[(?:%s)\]" % RETIRED_SECTIONS)

# 4. A TOOL EMITTING one. `"[promote]\n"` in a writer is how the ini corpus would grow one back
#    without any committed file carrying it. The `\n` is what makes this a header rather than prose:
#    a section name inside a sentence does not end the line.
PY_EMIT_RE = re.compile(r"\[(?:%s)\]\\n" % RETIRED_SECTIONS)

# 5. The differential oracle (F2D) and 6. the effect machinery + net_session probe (F2F), over src/
#    ONLY. tools/ legitimately still names these: gen_libmh_rebind.py's ARM_NS regex must recognise a
#    `shadow_arm::` namespace to strip it out of the FROZEN seam index, gen_va_census.py carries a
#    `shadow_arm` data field, and check_fork_d8.py is built out of the effects vocabulary. Those are
#    F2D/F2F's recorded tombstones; check_fork_d8 owns the tools side with a precise vocabulary.
SHADOW_RE = re.compile(r"MH_SHADOW|\bshadow_arm\b|mh::shadow::|mh_shadow")
PROBE_RE = re.compile(r"mh_effects|\beffecttest\b|\bshadowtest\b|net_session_probe|check_netprobe")

# 7. The tactical promotion registry (F2E) and 8. the rebind GATE's old spelling. `rebind_arming.cpp`
#    is the survivor; `rebind_gates` / `load_gates` named the file and the entry point that read the
#    deleted section, so their return would mean the gate came back with it.
TACT_RE = re.compile(
    r"tact_promote|\bpromote_site\b|\bpromote_register\b|\bpromote_say\b|\bpromote_report\b"
    r"|frame_entry_thunk|register_promotion_frame|install_promotion_frame_direct"
    r"|unit_enqueue_command_entry_thunk"
)
GATE_RE = re.compile(r"\brebind_gates\b|\bload_gates\b")

# 9. THE OLD HARNESS FILE (F2G / plan D12). mh_net.ini and mh_harness.ini merged; the harness arms
#    on `[harness] enable=1` and a leftover mh_harness.ini beside the exe is REFUSED by the DLL.
#    What this forbids is a WRITER or a PATH CONSTRUCTOR bringing the second file back -- a string
#    literal naming it in a path-shaped context, i.e. built into a path or opened. Prose that
#    NAMES the file is everywhere and must stay: the refusal itself, its message, this fork's
#    history, and the sweep calls that DELETE a stale one. So the pattern is deliberately not
#    "the word": it is the file name inside a format string that composes a path (`%smh_harness.ini`,
#    `.../mh_harness.ini`) or an os.path.join/scp destination -- and the deletes are exempted by
#    being `del /q`/os.remove shapes, which carry no separator or format directive before the name.
HARNESS_FILE_RE = re.compile(r'["\']\s*%s\s*mh_harness\.ini|/mh_harness\.ini')
# ONE FILE IS EXEMPT FROM ASSERTION 9, and it is the refusal itself: mh/config/config.h has to
# compose `%smh_harness.ini` in order to NOTICE one and kill the run. A gate that reds on the
# mechanism enforcing the very rule it checks is not stricter, it is wrong -- the same shape the
# nine allowed-survivor green arms below exist to prevent. Exempt by exact relative path, not by
# directory, so a second composer added in that folder tomorrow is still caught.
HARNESS_FILE_EXEMPT = {"mh/config/config.h"}

# 10. THE F5H RESIDUE SWEEP (2026-09-14). Three names retired by the sweep that closes out what F2E
#     and F4E left behind, and they are here rather than in a gate of their own because they retire
#     for the SAME cause as assertions 1-8: F2E demoted tactical mode, so everything that existed to
#     promote it is residue, and residue grows back by being pasted from a session that predates the
#     ruling. What is forbidden:
#       * MH_Harness_RebindTactFrame / RebindTactEnqueue -- the two callerless rebind setters. F2E
#         took their callers with the 18 tactical MH_EXPORT_REPLACE installs; F4E measured them
#         callerless and parked the delete here. The DETOURS they fed are untouched, so a real
#         tactical promotion re-adds a setter deliberately rather than by a paste.
#       * migration_ab's tact VEHICLE -- _TactArgs / TactRig / ab_one_tact / tact_plan. Dead by
#         registry since F2E left promote_modules.json one strategic row, so no configuration could
#         reach it. A re-import from tools/oneoff/ is how it would come back (one did; F5H deleted
#         that oneoff -- and note SKIP_DIRS excludes `oneoff`, so this assertion would NOT have seen
#         it; the ban is on a LIVE importer).
#       * the three deleted orphan uiscripts. A .txt basename only ever appears as a REFERENCE, so
#         here the ban really is the word -- a registry row or runner naming one would name a file
#         that is not there, and the suite's failure mode for that is a timeout, not a message.
#     Same narrowing doctrine as 2-4: the two C symbols and the four Python ones are forbidden as a
#     DEFINITION, a CALL or a dotted import -- never as the bare word -- because the tombstone
#     comments this sweep left behind name all six on purpose, and so does migration_ab's docstring.
F5H_RE = re.compile(
    r"\bMH_Harness_RebindTact(?:Frame|Enqueue)\s*\("
    r"|\b(?:class|def)\s+(?:_TactArgs|TactRig|ab_one_tact|tact_plan)\b"
    r"|\b(?:_TactArgs|TactRig|ab_one_tact|tact_plan)\s*\("
    r"|\bmigration_ab\.(?:_TactArgs|TactRig|ab_one_tact|tact_plan)\b"
    r"|\bmp_host_leave\.txt\b|\bmp_client_watch_leave\.txt\b|\bmp_host_dropmsg\.txt\b"
)

STRIP_LINE_COMMENT = re.compile(r"//.*$")
STRIP_PY_COMMENT = re.compile(r"#.*$")
STRIP_INI_COMMENT = re.compile(r";.*$")

SCAN_EXTS = (".cpp", ".h", ".py", ".ini", ".vcxproj")
SKIP_DIRS = {"Debug", "Release", "Win32", "attic", ".vs", "__pycache__", "oneoff"}

# THIS FILE spells every forbidden token -- it is built out of them -- so it excludes itself by
# absolute path, exactly as check_fork_d8 does. The selftest is what closes that hole.
SELF = os.path.abspath(__file__)
# ...and so does check_fork_d8.py, for the effects half. Excluded BY NAME rather than by a blanket
# "skip tools/check_fork_*", so a third gate added tomorrow is scanned rather than silently exempt.
ALLOWED_FILES = {os.path.join(TOOLS, "check_fork_d8.py")}


def _walk(root):
    for dirpath, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for f in files:
            if f.endswith(SCAN_EXTS):
                p = os.path.join(dirpath, f)
                a = os.path.abspath(p)
                if a != SELF and a not in ALLOWED_FILES:
                    yield p


def _rel(path, root):
    return os.path.relpath(path, root).replace("\\", "/")


def _code_lines(path):
    """Lines with line comments stripped (//, # for .py, ; for .ini).

    Block comments and docstrings are left in place, for check_fork_d8's reason: stripping /* */ or
    triple quotes without a parser risks eating code, and a false hit from prose fails LOUD, which is
    the safe direction. Every assertion here is narrow enough that prose does not trip it -- that is
    what assertions 2-4 being about a CALL, a LINE-ANCHORED HEADER and an EMITTED literal buys.
    """
    if path.endswith(".py"):
        strip = STRIP_PY_COMMENT
    elif path.endswith(".ini"):
        strip = STRIP_INI_COMMENT
    else:
        strip = STRIP_LINE_COMMENT
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            yield strip.sub("", line)


def scan(roots):
    """-> (hits, files_seen). `hits[assertion]` is a sorted set of "rel:line" strings.

    `files_seen` exists so a scan that visited nothing can be told apart from a clean tree -- the
    only anchor a zero-survivor check has.
    """
    hits = {
        k: set()
        for k in (
            "ship",
            "profile",
            "ini",
            "emit",
            "shadow",
            "probe",
            "tact",
            "gate",
            "harnfile",
            "f5h",
        )
    }
    files_seen = 0
    for root in roots:
        if not os.path.isdir(root):
            continue
        in_src = os.path.abspath(root) == os.path.abspath(SRC) or root.endswith("mh_dll")
        for path in _walk(root):
            files_seen += 1
            rel = _rel(path, root)
            is_py, is_ini = path.endswith(".py"), path.endswith(".ini")
            for n, line in enumerate(_code_lines(path), 1):
                where = "%s:%d" % (rel, n)
                if SHIP_RE.search(line):
                    hits["ship"].add(where)
                if PROFILE_RE.search(line):
                    hits["profile"].add(where)
                if is_ini and INI_SECTION_RE.search(line):
                    hits["ini"].add(where)
                if is_py and PY_EMIT_RE.search(line):
                    hits["emit"].add(where)
                if TACT_RE.search(line):
                    hits["tact"].add(where)
                if GATE_RE.search(line):
                    hits["gate"].add(where)
                if HARNESS_FILE_RE.search(line) and rel not in HARNESS_FILE_EXEMPT:
                    hits["harnfile"].add(where)
                if F5H_RE.search(line):
                    hits["f5h"].add(where)
                # The F2D/F2F vocabulary is a src/-only assertion; see the regex banner.
                if in_src:
                    if SHADOW_RE.search(line):
                        hits["shadow"].add(where)
                    if PROBE_RE.search(line):
                        hits["probe"].add(where)
    return hits, files_seen


MESSAGES = {
    "ship": "assertion 1: a per-mechanism ship constant is back (SHIP_PROMOTE_* / "
    "SHIP_REBIND_DEFAULT were deleted at F2E; the answer is mh::config::ours_run())",
    "profile": "assertion 2: a RETIRED ini section is READ again -- the installer itself. Which "
    "bodies run is `[config] mode` and nothing else (mh/config/config.h)",
    "ini": "assertion 3: a committed .ini carries a RETIRED section header. The DLL REFUSES such a "
    "run by name, so this fragment would kill every lane that merged it",
    "emit": "assertion 4: a tool WRITES a retired section header into an ini. The corpus would grow "
    "the vocabulary back without any committed file carrying it",
    "shadow": "assertion 5: the differential-oracle vocabulary is back in src/ (deleted at F2D)",
    "probe": "assertion 6: the deferred-effect / net_session-probe vocabulary is back in src/ "
    "(deleted at F2F)",
    "tact": "assertion 7: the tactical PROMOTION registry is back (deleted at F2E -- tactical mode "
    "is the game's own body in every configuration this fork ships)",
    "gate": "assertion 8: the rebind GATE is back under its old name. The arm bitmap survives as "
    "state/rebind_arming.cpp (arm_from_config / arm_none); `load_gates` read the deleted section",
    "harnfile": "assertion 9: something COMPOSES a path to the old mh_harness.ini. The file merged into mh_net.ini at F2G and the DLL now REFUSES a run that still has one beside the exe, so a writer that recreates it would kill every lane it touched",
    "f5h": "assertion 10: a name F5H RETIRED is back -- one of the two callerless tactical rebind "
    "setters, migration_ab's tact A/B vehicle, or one of the three deleted orphan uiscripts. All "
    "three families are residue of F2E's permanent tactical demotion; a definition, a call or a "
    "dotted import is what this forbids, never the bare word",
}


def check(roots=(SRC, TOOLS), say=print):
    hits, files_seen = scan(roots)
    fails = []

    if not files_seen:
        fails.append(
            "the scan visited ZERO files (%s) -- this run proves nothing; it is a broken or "
            "mis-rooted walker, not a clean tree." % ", ".join(roots)
        )

    for key in (
        "ship",
        "profile",
        "ini",
        "emit",
        "shadow",
        "probe",
        "tact",
        "gate",
        "harnfile",
        "f5h",
    ):
        if hits[key]:
            fails.append("%s: %s" % (MESSAGES[key], sorted(hits[key])))

    for f in fails:
        say("[FAIL] %s" % f)
    if not fails:
        say(
            "check_fork_f2_drop: PASS -- zero survivors of the F2 per-row configuration vocabulary "
            "over %d scanned file(s) in src/ + tools/" % files_seen
        )
    return 1 if fails else 0


def _selftest_root(prefix):
    """One root for every temp tree the selftest makes, removed at interpreter exit.

    The selftest used to mkdtemp per synthetic tree and never remove any of them: measured
    2026-09-18 at 21,462 leaked f2drop_* dirs in %TEMP% (with d8_*, d5hooks_*, inmem_selftest_*
    and narration_selftest_* alongside) -- one lint run leaks a few, and the lint runs every
    session. mkdtemp(dir=root) keeps every tree under one directory that atexit removes.
    """
    import atexit
    import shutil

    root = tempfile.mkdtemp(prefix=prefix)
    atexit.register(shutil.rmtree, root, ignore_errors=True)
    return root


def selftest():
    import tempfile

    ok = True

    def expect(name, cond):
        nonlocal ok
        print("  [%s] %s" % ("ok" if cond else "FAIL", name))
        ok = ok and cond

    sink = []
    rc = check((SRC, TOOLS), say=sink.append)
    expect("real tree passes (zero survivors)", rc == 0)
    if rc:
        for line in sink:
            print("    %s" % line)

    _, seen = scan((SRC, TOOLS))
    expect("the walker actually visits the real tree", seen > 100)

    root = _selftest_root("f2drop_selftest_")

    def synth(rel, content):
        d = tempfile.mkdtemp(prefix="f2drop_", dir=root)
        # `mh_dll` in the name so the src-only assertions (5, 6) treat it as the src tree.
        p = os.path.join(d, "mh_dll", rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(content)
        return os.path.join(d, "mh_dll")

    quiet = lambda *_: None  # noqa: E731

    expect(
        "an empty synthetic tree is REFUSED, not passed",
        check((tempfile.mkdtemp(prefix="f2drop_empty_", dir=root),), say=quiet) == 1,
    )
    expect(
        "a synthetic tree with an unrelated file passes",
        check((synth("libmh/sim/ok.cpp", "int x = 1;\n"),), say=quiet) == 0,
    )
    expect(
        "planted ship constant -> red",
        check((synth("libmh/sim/rogue.cpp", "int d = SHIP_PROMOTE_UNIT_TICK;\n"),), say=quiet) == 1,
    )
    expect(
        "planted SHIP_REBIND_DEFAULT -> red",
        check((synth("libmh/sim/rogue.cpp", "int d = SHIP_REBIND_DEFAULT;\n"),), say=quiet) == 1,
    )
    expect(
        "planted [promote] ini READ -> red",
        check(
            (synth("libmh/sim/rogue.cpp", 'GetPrivateProfileIntA("promote", "x", 0, p);\n'),),
            say=quiet,
        )
        == 1,
    )
    expect(
        "planted [rebind] section READ -> red",
        check(
            (synth("libmh/sim/rogue.cpp", 'GetPrivateProfileSectionA("rebind", b, n, p);\n'),),
            say=quiet,
        )
        == 1,
    )
    expect(
        "planted [state_handler_skip] read -> red",
        check(
            (
                synth(
                    "libmh/sim/rogue.cpp", 'GetPrivateProfileIntA("state_handler_skip", n, 0, p);\n'
                ),
            ),
            say=quiet,
        )
        == 1,
    )
    expect(
        "planted retired section in an ini -> red",
        check((synth("frag.ini", "[promote]\nlockstep=1\n"),), say=quiet) == 1,
    )
    expect(
        "planted [shadow] section in an ini -> red",
        check((synth("frag.ini", "[shadow]\nsome_site=1\n"),), say=quiet) == 1,
    )
    expect(
        "planted section EMITTER in a tool -> red",
        check((synth("writer.py", 'ini += "[rebind]\\n"\n'),), say=quiet) == 1,
    )
    expect(
        "planted shadow vocabulary in src -> red",
        check((synth("libmh/sim/rogue.cpp", "MH_SHADOW_REPLACE(fn, arm)\n"),), say=quiet) == 1,
    )
    expect(
        "planted effects/probe vocabulary in src -> red",
        check((synth("libmh/sim/rogue.cpp", "#include \"addr/mh_effects.gen.h\"\n"),), say=quiet)
        == 1,
    )
    expect(
        "planted tact promotion registry -> red",
        check((synth("libmh/tact/rogue.cpp", "#include \"tact/tact_promote.h\"\n"),), say=quiet)
        == 1,
    )
    expect(
        "planted rebind GATE name -> red",
        check((synth("libmh/state/rogue.cpp", "mh::rebind::load_gates(p, 1);\n"),), say=quiet) == 1,
    )

    # ---- F2G's three retired sections and the old harness FILE ------------------------------
    for _sec in ("pacing", "test", "probe"):
        expect(
            "planted [%s] READ -> red (F2G)" % _sec,
            check(
                (
                    synth(
                        "mh/seams/rogue.cpp",
                        'int v = GetPrivateProfileIntA("%s", "k", 0, ini);\n' % _sec,
                    ),
                ),
                say=quiet,
            )
            == 1,
        )
        expect(
            "planted [%s] SECTION HEADER in a committed ini -> red (F2G)" % _sec,
            check((synth("frag.ini", "[%s]\nk=1\n" % _sec),), say=quiet) == 1,
        )
        expect(
            "a tool EMITTING [%s] -> red (F2G)" % _sec,
            check(
                (synth("rogue.py", 'f.write("[%s]\\nk=1\\n")\n' % _sec),),
                say=quiet,
            )
            == 1,
        )
    expect(
        "planted mh_harness.ini PATH CONSTRUCTOR -> red (F2G)",
        check(
            (synth("mh/seams/rogue.cpp", 'wsprintfA(p, "%smh_harness.ini", dir);\n'),),
            say=quiet,
        )
        == 1,
    )
    expect(
        "planted mh_harness.ini scp DESTINATION -> red (F2G)",
        check(
            (synth("rogue.py", 'scp(key, vh, dst + "/mh_harness.ini")\n'),),
            say=quiet,
        )
        == 1,
    )

    # ---- ASSERTION 10 (F5H): each of the three retired families, planted in the shape it would
    # actually come back in -- a re-added setter, a re-imported vehicle, a registry row naming a
    # deleted script.
    expect(
        "planted tactical rebind SETTER -> red (F5H)",
        check(
            (
                synth(
                    "mh/seams/rogue.cpp",
                    'extern "C" int MH_Harness_RebindTactFrame(void *ours) { return 0; }\n',
                ),
            ),
            say=quiet,
        )
        == 1,
    )
    expect(
        "planted tact-vehicle DEFINITION -> red (F5H)",
        check((synth("rogue.py", "class TactRig:\n    pass\n"),), say=quiet) == 1,
    )
    expect(
        "planted tact-vehicle IMPORTER -> red (F5H)",
        check((synth("rogue.py", "rig = migration_ab.TactRig(dom, ab, a)\n"),), say=quiet) == 1,
    )
    expect(
        "planted registry row naming a DELETED uiscript -> red (F5H)",
        check((synth("rogue.py", '    "clients": ["mp_client_watch_leave.txt"],\n'),), say=quiet)
        == 1,
    )

    # ---- AND THE ALLOWED SURVIVORS MUST STAY GREEN. A gate that also reds on the thing the ruling
    # KEPT is not stricter, it is wrong -- and it would be discovered by someone deleting a live
    # mechanism to make lint pass. Each of these is a line the real tree actually contains.
    expect(
        "the arm-log's `; [promote] ...` prefix is NOT a hit",
        check(
            (
                synth(
                    "libmh/sim/ok.cpp", 'say("; [promote] sim_resid: ALL 32 seams installed\\n");\n'
                ),
            ),
            say=quiet,
        )
        == 0,
    )
    expect(
        "the arm-log's `; [rebind] ...` prefix is NOT a hit",
        check((synth("libmh/sim/ok.cpp", 'say("; [rebind] armed 676 row(s)\\n");\n'),), say=quiet)
        == 0,
    )
    expect(
        "MH_LIBMH_BIND is NOT a hit (ruling Q2: the BIND survives)",
        check((synth("libmh/sim/ok.cpp", "auto f = MH_LIBMH_BIND_map_unit_Add;\n"),), say=quiet)
        == 0,
    )
    expect(
        "mh::rebind::armed() is NOT a hit (the arm bitmap survives the gate)",
        check((synth("libmh/sim/ok.cpp", "if (::mh::rebind::armed(ROW_x)) {}\n"),), say=quiet) == 0,
    )
    expect(
        "arm_from_config / arm_none are NOT hits",
        check(
            (
                synth(
                    "libmh/sim/ok.cpp",
                    "mh::rebind::arm_from_config(true); mh::rebind::arm_none();\n",
                ),
            ),
            say=quiet,
        )
        == 0,
    )
    expect(
        "MF_MEASURED is NOT a hit (the renamed write-census flag)",
        check((synth("mh/addr/ok.h", "unsigned f = MF_MEASURED;\n"),), say=quiet) == 0,
    )
    expect(
        "a `[config] mode` read is NOT a hit (it is the replacement)",
        check(
            (
                synth(
                    "libmh/sim/ok.cpp",
                    'GetPrivateProfileStringA("config", "mode", "brokered", v, n, p);\n',
                ),
            ),
            say=quiet,
        )
        == 0,
    )
    expect(
        "a // comment naming a retired section is not a hit",
        check((synth("libmh/sim/ok.cpp", "// [promote] lockstep used to gate this\n"),), say=quiet)
        == 0,
    )
    expect(
        "a ; comment in an ini naming a retired section is not a hit",
        check(
            (synth("frag.ini", "; [promote] was deleted at F2E\n[config]\nmode=original\n"),),
            say=quiet,
        )
        == 0,
    )
    expect(
        "[uitest] / [video] are NOT hits (F2G moved `lane` and `fps_cap` INTO them)",
        check(
            (synth("frag.ini", "[uitest]\nlane=3\n\n[video]\nfps_cap=60\n"),),
            say=quiet,
        )
        == 0,
    )
    expect(
        "a `[uitest] lane` read is NOT a hit (the section `test` moved into)",
        check(
            (
                synth(
                    "mh/seams/ok.cpp",
                    'int n = GetPrivateProfileIntA("uitest", "lane", 0, g_ini);\n',
                ),
            ),
            say=quiet,
        )
        == 0,
    )
    expect(
        "the REFUSAL's own compose (mh/config/config.h) is exempt, and only it",
        check(
            (
                synth(
                    "mh/config/config.h",
                    'wsprintfA(stray, "%smh_harness.ini", dir);\n',
                ),
            ),
            say=quiet,
        )
        == 0,
    )
    expect(
        "F5H's own TOMBSTONE prose is NOT a hit -- the bare names must survive",
        check(
            (
                synth(
                    "rogue.py",
                    "'''MH_Harness_RebindTactFrame and RebindTactEnqueue stood here; "
                    "_TactArgs / TactRig / ab_one_tact / tact_plan were deleted at F5H.'''\n",
                ),
            ),
            say=quiet,
        )
        == 0,
    )
    expect(
        "a surviving orphan uiscript is NOT a hit -- only the three deleted names are banned",
        check((synth("rogue.py", '    "script": "mp_host_overlay.txt",\n'),), say=quiet) == 0,
    )
    expect(
        "SWEEPING a stale mh_harness.ini is NOT a hit -- the deletes must survive",
        check(
            (
                synth(
                    "rogue.py",
                    'os.remove(os.path.join(hd, "mh_harness.ini"))\n'
                    'remote(a, ip, "del /q %s\\\\mh_harness.ini 2>nul" % d)\n',
                ),
            ),
            say=quiet,
        )
        == 0,
    )

    print("check_fork_f2_drop --selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    return check()


if __name__ == "__main__":
    sys.exit(main())
