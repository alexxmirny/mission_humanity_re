#!/usr/bin/env python3
"""check_fork_d8.py -- fork ruling D8's gate: the deferred-effect machinery STAYS deleted.

D8 (the fork plan) dropped effecttest + the deferred-effect machinery together at F2, gated on
this check. Two lives, one file:

  F1C (2026-09-12) -- the PRE-DROP shape. It pinned the closure while the machinery still existed:
      one gen-header includer, a five-file effects.h seam set, and a POSITIVE LIST of adjudicated
      mh::effects:: API sites. The premise it tested was found FALSE as written -- `[net]
      sync_gameover` registered a gate participant with no fallback, so the drop would have killed
      the U33 fix silently behind a success log -- and that is what made the drop safe to make.
  F2F (2026-09-12) -- THE POST-DROP shape, below. mh/effects/, mh/addr/mh_effects.gen.h,
      tools/gen_dll_effects.py and mh_nettest/effects_selftest.cpp are gone, the four guarded call  # CITATION-OK
      sites are unconditional own-detours, and this check flipped to ZERO SURVIVORS.

Four assertions, over src/ AND tools/ (gen_dll_effects.py was the generator; its vocabulary must
not come back through a tool either):

  1  `#include "addr/mh_effects.gen.h"`             -- NOWHERE.
  2  the deleted header's exported vocabulary        -- NOWHERE.
  3  any `mh::effects::` API call                    -- NOWHERE (API_ALLOWED is empty).
  4  `#include "effects/effects.h"`                  -- NOWHERE (EFFECTS_H_BASELINE is empty).

THE BROKEN-GREP PROBLEM, and why this file did not simply become four `assert not found`. Before
the drop the scans could prove themselves on the real tree: the known referencers had to be found
or the run failed. A zero-survivor scan has no such anchor -- a regex that matches nothing and a
regex that is broken produce the identical clean result, which is the exact shape a
negative-claim gate exists to refuse. So the controls moved into the selftest and became PLANT-IN-A-TEMP-TREE controls: for
each assertion a synthetic tree carrying that one violation must go RED, which exercises the same
regex on the same walker as the real run. An additional arm asserts the real tree's walker actually
VISITED files (a scan over an empty/mis-rooted tree is refused by name, not read as clean).

usage:
  python tools/check_fork_d8.py            # run the four assertions (exit 1 on violation)
  python tools/check_fork_d8.py --selftest # planted-violation reds + the walker-liveness control
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src", "mh_dll")
TOOLS = os.path.join(REPO, "tools")

GEN_INCLUDE_RE = re.compile(r'#include\s+"addr/mh_effects\.gen\.h"')
VOCAB_RE = re.compile(
    r"MH_EFFECTS_TARGET_COUNT|MH_EFFECTS_UNGATED_COUNT|MH_EFFECTS_IMPL"
    r"|mh_effects_(?:install_all|note_ungated_all|target_|gate_|thunk_)"
)
API_RE = re.compile(
    r"mh::effects::(participate|gate_armed|enter_deferred|install_all|report_participants"
    r"|begin_window|switch_to_ours|end_window|probe_step|tact_probe_frame|probe_exactly_once"
    r"|probe_tact_both_directions|defer_entry|note_ungated|enter|leave)"
)
EFFECTS_H_RE = re.compile(r'#include\s+"effects/effects\.h"')

# Assertion 3's adjudication. EMPTY at F2F -- the API no longer exists, so there is no such thing as
# an adjudicated call site any more. A row here would mean the drop had been partially reverted.
API_ALLOWED = {}

# Assertion 4's pinned baseline. EMPTY at F2F, for the same reason.
EFFECTS_H_BASELINE = set()

STRIP_LINE_COMMENT = re.compile(r"//.*$")
STRIP_PY_COMMENT = re.compile(r"#.*$")

# What the walker reads. The tools tree is included because gen_dll_effects.py lived there and
# emitted every token above; a regenerated copy would be as much a survivor as a C++ one.
SCAN_EXTS = (".cpp", ".h", ".py")
SKIP_DIRS = {"Debug", "Release", "Win32", "attic", ".vs", "__pycache__", "data", "uiscripts"}


# THIS FILE. It is the one place in the tree that must SPELL the forbidden vocabulary -- it is
# built out of it -- so it excludes itself by absolute path. That is a real hole in the direct scan,
# and the selftest is what closes it: every regex is re-fired against a planted violation in a temp
# tree, through this same walker, so a regex that stopped matching fails there.
SELF = os.path.abspath(__file__)


def _walk(root):
    for dirpath, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for f in files:
            if f.endswith(SCAN_EXTS):
                p = os.path.join(dirpath, f)
                if os.path.abspath(p) != SELF:
                    yield p


def _rel(path, root):
    return os.path.relpath(path, root).replace("\\", "/")


def _code_lines(path):
    """Lines with // (or #, for .py) comments stripped. Block comments are left in place: every
    surviving use of this vocabulary in prose is a `//` banner or a docstring line, and stripping
    /* */ or triple quotes without a parser risks eating code. A false hit from a block comment
    fails LOUD, which is the safe direction."""
    strip = STRIP_PY_COMMENT if path.endswith(".py") else STRIP_LINE_COMMENT
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            yield strip.sub("", line)


def scan(roots):
    """Return (gen_includers, vocab_hits, api_hits, effects_h, files_seen) over one or more trees.

    `files_seen` exists so a scan that visited nothing can be told apart from a clean tree -- the
    only anchor a zero-survivor check has.
    """
    gen_includers, vocab_hits, api_hits, effects_h = set(), set(), set(), set()
    files_seen = 0
    for root in roots:
        if not os.path.isdir(root):
            continue
        for path in _walk(root):
            files_seen += 1
            rel = _rel(path, root)
            for line in _code_lines(path):
                if GEN_INCLUDE_RE.search(line):
                    gen_includers.add(rel)
                if VOCAB_RE.search(line):
                    vocab_hits.add(rel)
                if EFFECTS_H_RE.search(line):
                    effects_h.add(rel)
                for m in API_RE.finditer(line):
                    api_hits.add((rel, m.group(1)))
    return gen_includers, vocab_hits, api_hits, effects_h, files_seen


def check(roots=(SRC, TOOLS), say=print):
    gen_includers, vocab_hits, api_hits, effects_h, files_seen = scan(roots)
    fails = []

    if not files_seen:
        fails.append(
            "the scan visited ZERO files (%s) -- this run proves nothing; it is a broken or "
            "mis-rooted walker, not a clean tree." % ", ".join(roots)
        )

    if gen_includers:
        fails.append(
            "assertion 1: addr/mh_effects.gen.h is INCLUDED again (the header was deleted at "
            "F2F): %s" % sorted(gen_includers)
        )

    if vocab_hits:
        fails.append(
            "assertion 2: the deleted generated header's vocabulary is back: %s"
            % sorted(vocab_hits)
        )

    unadjudicated = {h for h in api_hits if h not in API_ALLOWED}
    if unadjudicated:
        fails.append(
            "assertion 3: mh::effects:: API call(s) survive the F2F drop -- the namespace no "
            "longer exists, so this is either a partial revert or a new dependency on a deleted "
            "mechanism: %s" % sorted("%s :: %s" % h for h in unadjudicated)
        )

    if effects_h != EFFECTS_H_BASELINE:
        fails.append(
            "assertion 4: effects/effects.h is INCLUDED again (the header was deleted at F2F): %s"
            % sorted(effects_h)
        )

    for f in fails:
        say("[FAIL] %s" % f)
    if not fails:
        say(
            "check_fork_d8: PASS -- zero survivors of the deferred-effect machinery over %d "
            "scanned file(s) in src/ + tools/" % files_seen
        )
    return 1 if fails else 0


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

    # THE CONTROL A ZERO-SURVIVOR SCAN NEEDS. Before the drop the assertions anchored themselves on
    # finding the known referencers; with the referencers deleted, the only way to tell a working
    # scan from a dead one is that it visited files at all, and that each regex still reds on a
    # planted violation run through the same walker.
    _, _, _, _, seen = scan((SRC, TOOLS))
    expect("the walker actually visits the real tree", seen > 100)

    def synth(rel, content):
        d = tempfile.mkdtemp(prefix="d8_selftest_")
        p = os.path.join(d, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(content)
        return d

    quiet = lambda *_: None  # noqa: E731

    expect(
        "an empty synthetic tree is REFUSED, not passed",
        check((tempfile.mkdtemp(prefix="d8_empty_"),), say=quiet) == 1,
    )
    expect(
        "a synthetic tree with an unrelated file passes",
        check((synth("libmh/sim/ok.cpp", "int x = 1;\n"),), say=quiet) == 0,
    )
    expect(
        "planted gen-header include -> red",
        check((synth("libmh/sim/rogue.cpp", '#include "addr/mh_effects.gen.h"\n'),), say=quiet)
        == 1,
    )
    expect(
        "planted vocabulary use -> red",
        check((synth("libmh/sim/rogue.cpp", "int n = MH_EFFECTS_TARGET_COUNT;\n"),), say=quiet)
        == 1,
    )
    expect(
        "planted API call -> red",
        check((synth("libmh/sim/rogue.cpp", "auto p = mh::effects::participate;\n"),), say=quiet)
        == 1,
    )
    expect(
        "planted shadow-window API call -> red",
        check((synth("libmh/sim/rogue.cpp", 'mh::effects::begin_window("x");\n'),), say=quiet) == 1,
    )
    expect(
        "planted effects.h include -> red",
        check((synth("libmh/sim/rogue.cpp", '#include "effects/effects.h"\n'),), say=quiet) == 1,
    )
    expect(
        "planted generator vocabulary in a TOOL -> red",
        check((synth("regen_effects.py", 'OUT = "MH_EFFECTS_IMPL"\n'),), say=quiet) == 1,
    )
    # ...and the comment-stripping is real: prose mentions must NOT trip assertion 3. This whole
    # repo's history talks about the mechanism; the gate is about code.
    expect(
        "a // comment mentioning the API is not a hit",
        check((synth("libmh/sim/rogue.cpp", "// see mh::effects::participate\n"),), say=quiet) == 0,
    )
    expect(
        "a # comment in a tool mentioning the API is not a hit",
        check((synth("note.py", "# mh::effects::install_all was deleted at F2F\n"),), say=quiet)
        == 0,
    )

    print("check_fork_d8 --selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    return check()


if __name__ == "__main__":
    sys.exit(main())
