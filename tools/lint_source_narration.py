#!/usr/bin/env python3
"""Ratchet lint against session-diary narration in src/mh_dll comments.

WHY (2026-08-26). Unattended migration sessions read source files (the state headers, the exemplar
TUs, the install aggregators) and imitate what they see. When those files carry session-diary
comments -- "Ninth slice (2026-08-26)", "this session", "registered for the same reason the
thirteenth slice's row is" -- two failure modes compound: the next session copies the style
(tact_pilot.cpp reached 46% comment lines, mostly diary), and the diary's claims go stale the
moment the world moves, then get re-read as current state (the "blocked on TACT-CUT2" myth cost
five sessions). A comment should state a present-tense constraint the code cannot show; history
belongs in git blame and the session hand-off reports.

WHAT IT FLAGS. `//` comment lines under src/mh_dll, and `#` COMMENT LINES under tools/,
matching diary markers:
  - "this/that/next/prior/previous session", "session 20xx-..." (session ids)
  - slice ordinals: "first slice" ... "fourteenth slice", "3rd slice", "slice 7"
Dates alone are NOT flagged -- a date on a measured figure is provenance, not diary.
A line carrying `NARRATION-OK` is exempt (for the rare legit use of these words).

PYTHON IS SCANNED THROUGH `tokenize`, COMMENT TOKENS ONLY -- never docstrings or string
literals. That is not a shortcut, it is the only workable rule: THIS FILE's own docstring
says "this session" and "Ninth slice" because it has to quote what it bans, and half the
tools/ hits live inside operator messages and agent briefs that are talking ABOUT sessions --
a driver that prints "previous session report" is doing its job. A `#` comment is the place
where a Python file narrates its own history, and it is the only place this lint owns.

FROZEN ENTRIES (`_frozen` in the baseline). A path whose narration is not going to be drained
because the file LEAVES with the private layers gets a frozen entry carrying a mandatory
reason. Three rules keep it from becoming a dumping ground: `--update-baseline` NEVER writes
one, the path must be WITHHELD by tools/data/publish_ledger.json (freezing a published file is
refused), and a frozen path that is PRESENT and has reached zero hits FAILS -- a freeze cannot
outlive the coupling it excuses. The withheld test is a GLOB MATCH against the ledger, not a
`git ls-files` membership test, and a withheld path this checkout does not carry is silent
rather than failing twice: on the public cut the file is absent BY DESIGN, which is the freeze
reason coming true, not a defect (fork F5M -- before it, six frozen entries reddened every
public clone with the two contradictory messages "not withheld" and "now clean").

THE RATCHET. ~700 such lines pre-exist across ~300 files; rewriting them all at once is churn, so
the lint fails only a file whose count EXCEEDS its committed baseline
(tools/data/source_narration_baseline.json). New files start at baseline 0, so new diary anywhere
is an immediate failure. When a drain reduces a file, run `--update-baseline` in the same commit --
the baseline only moves down (this tool refuses to raise any count).

Usage:
  python tools/lint_source_narration.py                    # the check (lint_repo runs this)
  python tools/lint_source_narration.py --update-baseline  # tighten baseline to current counts
  python tools/lint_source_narration.py --top 15           # show the biggest offenders
  python tools/lint_source_narration.py --selftest         # the negative cases still fire
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tokenize

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src", "mh_dll")
TOOLS = os.path.join(REPO, "tools")
BASELINE = os.path.join(REPO, "tools", "data", "source_narration_baseline.json")

ORDINALS = (
    "first|second|third|fourth|fifth|sixth|seventh|eighth|ninth|tenth|"
    "eleventh|twelfth|thirteenth|fourteenth|fifteenth"
)
MARKER = re.compile(
    r"\b(?:this|that|next|prior|previous)\s+session\b"
    r"|\bsession\s+20\d\d"
    r"|\b(?:%s)\s+slice\b"
    r"|\b\d+(?:st|nd|rd|th)\s+slice\b"
    r"|\bslice\s+\d" % ORDINALS,
    re.IGNORECASE,
)
COMMENT = re.compile(r"^\s*//")
EXEMPT = "NARRATION-OK"


def scan_hits_line(line):
    """Does ONE line trip the lint? (comment + marker + not exempt) -- the per-line predicate."""
    return bool(COMMENT.match(line) and MARKER.search(line) and EXEMPT not in line)


def scan_file(path):
    """Line numbers of diary-marker comment lines in one file."""
    hits = []
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for i, line in enumerate(fh, 1):
                if scan_hits_line(line):
                    hits.append(i)
    except OSError:
        pass
    return hits


def scan_py_file(path):
    """Line numbers of diary-marker `#` COMMENT lines in one Python file. Comment tokens only --
    see the module docstring for why strings are out of scope. A syntactically broken file is
    reported as clean rather than crashing the gate; the build/ruff rows own that failure."""
    hits = []
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            src_lines = fh.readlines()
    except OSError:
        return hits
    try:
        with open(path, "rb") as fh:
            for tok in tokenize.tokenize(fh.readline):
                if tok.type != tokenize.COMMENT:
                    continue
                line = src_lines[tok.start[0] - 1] if tok.start[0] <= len(src_lines) else ""
                if tok.start[1] != len(line) - len(line.lstrip()):
                    continue  # a trailing comment on a code line is not narration
                if MARKER.search(tok.string) and EXEMPT not in tok.string:
                    hits.append(tok.start[0])
    except (tokenize.TokenError, SyntaxError, IndentationError, UnicodeDecodeError):
        return []
    return sorted(set(hits))


def scan_py_tree(root=TOOLS):
    """{repo-relative posix path: [line numbers]} for every .py under tools/."""
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in ("__pycache__", "attic")]
        for fn in filenames:
            if not fn.endswith(".py"):
                continue
            p = os.path.join(dirpath, fn)
            hits = scan_py_file(p)
            if hits:
                out[os.path.relpath(p, REPO).replace(os.sep, "/")] = hits
    return out


def scan_all():
    """Both trees, one map -- what the ratchet and the baseline are keyed on."""
    out = scan_tree()
    out.update(scan_py_tree())
    return out


def scan_tree(root=SRC):
    """{repo-relative posix path: [line numbers]} for every non-generated .cpp/.h under root."""
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in ("attic", "Release", "Debug", ".vs")]
        for fn in filenames:
            if not fn.endswith((".cpp", ".h")) or fn.endswith(".gen.h"):
                continue
            p = os.path.join(dirpath, fn)
            hits = scan_file(p)
            if hits:
                out[os.path.relpath(p, REPO).replace(os.sep, "/")] = hits
    return out


def _raw_baseline():
    try:
        with open(BASELINE, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {}


def load_baseline():
    return {k: v for k, v in _raw_baseline().items() if not k.startswith("_")}


def load_frozen():
    """{path: reason} -- entries the drain deliberately will not reach. A reason is MANDATORY."""
    fr = _raw_baseline().get("_frozen") or {}
    for p, why in sorted(fr.items()):
        if not str(why).strip():
            sys.exit(
                "narration baseline: frozen entry %s carries no reason -- a freeze without one "
                "is the dumping ground this mechanism exists to prevent." % p
            )
    return fr


def _withheld(paths):
    """The subset of `paths` the publish ledger does NOT publish -- BY GLOB MATCH against the
    ledger rules, NOT by membership of `git ls-files`.

    That distinction is the whole public-tree fix (fork F5M). A ledger rule is a statement about a
    PATH, and the one-off-scripts rule keeps saying `withhold` whether or not this checkout
    carries the file. The earlier trackedness test could not say that: on a cut tree `git ls-files`
    does not list those paths at all, so every frozen entry came back "not withheld" and the freeze
    arm fired on all six at once with a reason that was the exact opposite of the truth.

    Import-guarded: if the ledger machinery is absent, every path is treated as published, so a
    freeze fails loudly rather than passing silently."""
    try:
        sys.path.insert(0, os.path.join(REPO, "tools"))
        import check_publishable as cp  # noqa: PLC0415

        led = cp.load_ledger()
        by_path, _, _ = cp.classify(list(paths), led)
        return {p for p in paths if p in by_path and by_path[p]["disposition"] != "publish"}
    except Exception:
        return set()


def _present(paths):
    """The subset of `paths` this checkout actually carries on disk."""
    return {p for p in paths if os.path.isfile(os.path.join(REPO, p.replace("/", os.sep)))}


def check(tree=None, baseline=None, frozen=None, withheld=None, present=None):
    """(problem lines, ok). A file fails only when its count exceeds its baseline; a FROZEN path is
    exempt from that, and instead has to keep earning its freeze.

    THREE OUTCOMES PER FROZEN ENTRY, and the third is the one the public tree needs:
      * not withheld by the ledger          -> FAIL (a published file's narration gets drained)
      * withheld and PRESENT but at 0 hits  -> FAIL (a freeze may not outlive its coupling)
      * withheld and ABSENT                 -> SILENT. The file left with the private layers,
        which is precisely what its freeze reason says will happen. Neither failure applies: it is
        not published, and "now clean" would be a claim about a file this tree does not have."""
    tree = scan_all() if tree is None else tree
    baseline = load_baseline() if baseline is None else baseline
    frozen = load_frozen() if frozen is None else frozen
    probs = []
    if frozen:
        keys = sorted(frozen)
        wh = _withheld(keys) if withheld is None else set(withheld)
        here = _present(keys) if present is None else set(present)
        for path in keys:
            if path not in wh:
                probs.append(
                    "%s: FROZEN, but the publish ledger does not withhold it -- a published file's "
                    "narration has to be drained, not frozen" % path
                )
                continue
            if path not in here:
                continue  # withheld and absent: the cut tree, working as declared
            if not tree.get(path):
                probs.append(
                    "%s: FROZEN but now clean -- delete the frozen entry; a freeze may not "
                    "outlive the coupling it excuses" % path
                )
    for path, hits in sorted(tree.items()):
        if path in frozen:
            continue
        allowed = baseline.get(path, 0)
        if len(hits) > allowed:
            shown = ", ".join(str(n) for n in hits[:8]) + (" ..." if len(hits) > 8 else "")
            probs.append(
                "%s: %d diary-marker comment line(s), baseline %d (lines %s) -- state the "
                "constraint in present tense; history goes in the session report, not the code"
                % (path, len(hits), allowed, shown)
            )
    return probs, not probs


def update_baseline():
    tree = scan_all()
    frozen = load_frozen()
    counts = {p: len(h) for p, h in sorted(tree.items()) if p not in frozen}
    old = load_baseline()
    raised = [p for p, n in counts.items() if n > old.get(p, 0) and old]
    if raised:
        sys.exit(
            "refusing to RAISE the baseline for: %s\nThe ratchet only tightens -- rewrite the "
            "comment(s) instead (or mark a genuinely legit line NARRATION-OK)." % ", ".join(raised)
        )
    doc = {
        "_comment": "Ratchet baseline for tools/lint_source_narration.py: per-file counts of "
        "pre-existing session-diary comment lines under src/mh_dll (`//`) and tools/ (`#` comment "
        "tokens, added 2026-09-16 at fork F5E step 10). The lint fails a file that EXCEEDS its "
        "count; --update-baseline only lowers entries. Drain files toward 0 and regenerate in the "
        "same commit.",
        "_frozen_comment": "`_frozen` is NOT written by --update-baseline and is not a count: it "
        "is {path: reason} for files whose narration will never be drained because the file LEAVES "
        "with the private layers it serves. A frozen path must be withheld by the publish ledger "
        "and must still have at least one hit, or the lint fails -- see the module docstring.",
    }
    if frozen:
        doc["_frozen"] = dict(sorted(frozen.items()))
    doc.update(counts)
    with open(BASELINE, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, indent=2)
        fh.write("\n")
    dropped = sorted(set(old) - set(counts))
    print(
        "baseline updated: %d file(s), %d line(s) total%s"
        % (
            len(counts),
            sum(counts.values()),
            "; dropped %d now-clean file(s)" % len(dropped) if dropped else "",
        )
    )


def selftest():
    """The negative cases still fire -- on synthetic data, never the live tree."""
    fake = {"src/mh_dll/mh/x.cpp": [3, 9], "src/mh_dll/mh/y.h": [1]}  # CITATION-OK
    probs, ok = check(
        tree=dict(fake),
        baseline={"src/mh_dll/mh/x.cpp": 2},  # CITATION-OK
        frozen={},
    )
    assert not ok and len(probs) == 1 and "y.h" in probs[0], probs  # y.h over its implicit 0
    probs, ok = check(
        tree=dict(fake),
        baseline={"src/mh_dll/mh/x.cpp": 2, "src/mh_dll/mh/y.h": 1},  # CITATION-OK
        frozen={},
    )
    assert ok, probs  # at baseline everywhere -> clean
    probs, ok = check(
        tree={"src/mh_dll/mh/x.cpp": [1, 2, 3]},  # CITATION-OK
        baseline={"src/mh_dll/mh/x.cpp": 2},  # CITATION-OK
        frozen={},
    )
    assert not ok, "a file growing past its baseline must fail"
    assert MARKER.search("// Ninth slice (2026-08-26), registered here")
    assert MARKER.search("// left for the NEXT SESSION to drain")
    assert MARKER.search("// see session 2026-08-26-1449's report")
    assert MARKER.search("// 3rd slice of TACT1B")
    assert not MARKER.search("// MEASURED 2026-08-17: 158/782 calls diverged"), "dates alone pass"
    line = "// this slice of the array"  # 'slice' without ordinal/number does not trip
    assert not MARKER.search(line), line
    ln = "// this session NARRATION-OK: legit wording"
    assert not scan_hits_line(ln), "the NARRATION-OK escape must work"
    # tools/ scanning: a `#` comment trips, a docstring and a trailing comment do not
    import tempfile

    tmp = tempfile.mkdtemp(prefix="narration_selftest_")
    py = os.path.join(tmp, "plant.py")
    with open(py, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(
            '"""A docstring saying this session and third slice must NOT trip."""\n'
            "x = 1  # trailing this session comment: a code line, not narration\n"
            "# drained in the third slice of the sweep\n"
            "y = 'a string mentioning this session'\n"
            "# kept because it is NARRATION-OK: this session\n"
        )
    got = scan_py_file(py)
    assert got == [3], "tools/ scan must flag only the own-line # comment, got %r" % (got,)

    # _frozen: a published path may not be frozen, and a clean frozen path must be deleted
    FZ = "tools/oneoff/x.py"  # CITATION-OK
    probs, ok = check(
        tree={FZ: [2]},
        baseline={},
        frozen={FZ: "leaves with the private layers"},
        withheld={FZ},
        present={FZ},
    )
    assert ok, probs
    probs, ok = check(tree={FZ: [2]}, baseline={}, frozen={FZ: "r"}, withheld=set(), present={FZ})
    assert not ok and "does not withhold" in probs[0], probs
    probs, ok = check(tree={}, baseline={}, frozen={FZ: "r"}, withheld={FZ}, present={FZ})
    assert not ok and "now clean" in probs[0], probs
    # ...and the public-tree case (fork F5M): the file is ABSENT because it left with the private
    # layers. Withheld + absent is silent -- neither "not withheld" nor "now clean" is true of a
    # file this checkout does not carry. Without this arm the six real frozen entries produced
    # twelve contradictory problem lines on every public clone.
    probs, ok = check(tree={}, baseline={}, frozen={FZ: "r"}, withheld={FZ}, present=set())
    assert ok, probs
    # An ABSENT path the ledger does NOT withhold is still a failure: absence is not an excuse,
    # only the declared disposition is.
    probs, ok = check(tree={}, baseline={}, frozen={FZ: "r"}, withheld=set(), present=set())
    assert not ok and "does not withhold" in probs[0], probs
    # And the glob-matched disposition is what _withheld now answers with, present or not: the
    # real frozen roster must read as withheld even though nothing was passed in.
    real = sorted(load_frozen())
    if real:
        assert _withheld(real) == set(real), sorted(set(real) - _withheld(real))
    print("lint_source_narration selftest: PASS")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--top", type=int, metavar="N", help="show the N biggest offenders and exit")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        selftest()
        return
    if args.update_baseline:
        update_baseline()
        return
    if args.top:
        tree = scan_all()
        for p, h in sorted(tree.items(), key=lambda kv: -len(kv[1]))[: args.top]:
            print("%5d  %s" % (len(h), p))
        print("%5d  total over %d file(s)" % (sum(len(h) for h in tree.values()), len(tree)))
        return
    probs, ok = check()
    for p in probs:
        print("[narration] " + p)
    if not ok:
        sys.exit(1)
    print("lint_source_narration: PASS")


if __name__ == "__main__":
    main()
