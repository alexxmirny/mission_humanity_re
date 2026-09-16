#!/usr/bin/env python3
"""Keep the repo's markdown TABLES actually rendering as tables.

Why this exists. On 2026-08-03 the failure ledger -- the document every session is told to
consult before any "dead/unwired/absent" claim -- was found with **112 of its 192 rows (58%) not
rendering as a table at all**. It had been accumulating that way for weeks, silently, because the
source looks fine in an editor: every line starts with `|`. Only the rendered view shows the damage,
and nothing in the gate ever looked.

Worse than layout: an unescaped `|` inside a code span does not merely split the row, it DELETES the
character from the rendered text. Four rows were serving wrong instructions --
`FLAGS |= 0x40` rendered as `FLAGS = 0x40`, `if (now == -1 || next == -1)` as
`if (now == -1 next == -1)`, `grep ... | wc -l` as `grep ... wc -l`, and `|| true` as ` true`.
A reader had no way to tell.

The three failure modes, all silent in source:
  1. ORPHAN ROWS -- pipe-prefixed lines not preceded (in their run) by a header + delimiter row. GFM
     renders these as a paragraph of literal pipes. Caused by a blank line inside a table, prose
     between rows, or rows appended past the end of the document.
  2. RAGGED ROWS -- a row whose cell count differs from its header's. Usually an unescaped `|`.
  3. PIPES IN CODE SPANS -- `` `a | b` `` splits the row; backticks do NOT protect it in GFM.

Read-only. No Ghidra. Run from `tools/lint_repo.py`, or directly:
    python tools/lint_md_tables.py [PATH ...] [--selftest]
"""

import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SPLIT = re.compile(r"(?<!\\)\|")
DELIM = re.compile(r"^\|?[\s:|-]+\|?$")
CODE = re.compile(r"`([^`]*)`")


def cells(line):
    s = line.strip()
    if s.startswith("|"):
        s = s[1:]
    if s.endswith("|") and not s.endswith("\\|"):
        s = s[:-1]
    return SPLIT.split(s)


def check_text(text, label="<text>"):
    """Return a list of human-readable problems (empty == renders correctly)."""
    problems = []
    lines = text.splitlines()
    n = len(lines)

    def looks_like_row(i):
        return 1 <= i <= n and lines[i - 1].lstrip().startswith("|")

    def is_row(i):
        """A pipe-prefixed line that is really a TABLE row.

        An ISOLATED one is not. Wrapped prose can begin with a pipe -- a generated view is built
        from YAML whose text contains `|0xC0` and `|DAT_x - DAT_y|*2`, and a wrap put those at the
        start of a line. A real table is never one line long (it needs a header AND a delimiter), so
        an isolated pipe line is only treated as a row when it also CLOSES with a pipe, which prose
        does not.
        """
        if not looks_like_row(i):
            return False
        if looks_like_row(i - 1) or looks_like_row(i + 1):
            return True
        return lines[i - 1].rstrip().endswith("|")

    def is_delim(i):
        return is_row(i) and bool(DELIM.match(lines[i - 1].strip()))

    # Rows that GFM will actually render: a run opened by header + delimiter.
    rendering, widths = {}, {}
    for i in range(1, n + 1):
        if not is_delim(i) or not is_row(i - 1):
            continue
        w = len(cells(lines[i - 2]))
        j = i + 1
        while is_row(j):
            rendering[j] = i
            widths[j] = w
            j += 1

    for i in range(1, n + 1):
        if not is_row(i) or is_delim(i):
            continue
        if i + 1 <= n and is_delim(i + 1):
            continue  # this is a header
        if i not in rendering:
            problems.append(
                "%s:%d: row renders as PROSE (no header+delimiter opens its run)" % (label, i)
            )
            continue
        got = len(cells(lines[i - 1]))
        if got != widths[i]:
            problems.append(
                "%s:%d: %d cells, header has %d -- ragged row" % (label, i, got, widths[i])
            )

    for i in range(1, n + 1):
        if not is_row(i):
            continue
        for m in CODE.finditer(lines[i - 1]):
            if re.search(r"(?<!\\)\|", m.group(1)):
                problems.append(
                    "%s:%d: unescaped `|` inside a code span -- GFM splits the row AND drops the "
                    "character: `%s`" % (label, i, m.group(1)[:50])
                )
    return problems


def selftest():
    """The gate has to be able to go RED, on each mode separately."""
    hdr = "| A | B |\n| --- | --- |\n"
    failures = []

    if check_text(hdr + "| x | y |\n"):
        failures.append("a well-formed table was rejected (gate stuck red)")
    if not any("PROSE" in p for p in check_text(hdr + "| x | y |\n\n| orphan | row |\n")):
        failures.append("a blank line splitting a table was NOT caught")
    if not any("PROSE" in p for p in check_text("| no | header |\n")):
        failures.append("a headerless row block was NOT caught")
    if not any("ragged" in p for p in check_text(hdr + "| a | b | c |\n")):
        failures.append("a ragged row was NOT caught")
    if not any("code span" in p for p in check_text(hdr + "| `a | b` | y |\n")):
        failures.append("a pipe inside a code span was NOT caught")
    # ... and an ESCAPED pipe in a code span is legal, or the rule is unusable
    if check_text(hdr + "| `a \\| b` | y |\n"):
        failures.append("an escaped pipe was rejected (the fix must be accepted)")
    # A WRAPPED PROSE line that merely begins with a pipe is not a row. Generated views wrap YAML
    # text, and `|DAT_x - DAT_y|*2` at a line start is prose, not a table.
    if check_text("some prose about\n|0xC0 = bit7 HIDDEN, toggled per frame in\nmore prose\n"):
        failures.append("wrapped prose starting with `|` was misread as a table row")
    # ... but a genuinely orphaned row -- which CLOSES with a pipe -- must still be caught
    if not any("PROSE" in p for p in check_text("prose\n| orphan | row |\nprose\n")):
        failures.append("an isolated but well-formed orphan row was NOT caught")

    for f in failures:
        print("selftest FAIL: " + f)
    if not failures:
        print("lint_md_tables selftest: 8/8 arms behave")
    return 1 if failures else 0


# An operational default path list, not a citation: a path the tree does not carry is skipped.
DEFAULT_PATHS = ["docs", "NOTES.md", "TASKS.md", "ROADMAP.md", "CLAUDE.md", "tasks"]  # CITATION-OK


def iter_files(paths):
    for p in paths:
        full = p if os.path.isabs(p) else os.path.join(REPO, p)
        if os.path.isdir(full):
            for root, _dirs, files in os.walk(full):
                for f in sorted(files):
                    if f.endswith(".md"):
                        yield os.path.join(root, f)
        elif os.path.isfile(full):
            yield full


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="*", default=None)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    problems, scanned = [], 0
    for path in iter_files(args.paths or DEFAULT_PATHS):
        with open(path, "r", encoding="utf-8") as fh:
            text = fh.read()
        if "|" not in text:
            continue
        scanned += 1
        rel = os.path.relpath(path, REPO).replace("\\", "/")
        problems.extend(check_text(text, rel))

    if problems:
        print("markdown tables: %d problem(s)" % len(problems))
        for p in problems[:40]:
            print("  " + p)
        if len(problems) > 40:
            print("  ... and %d more" % (len(problems) - 40))
        print("  A row that does not render is a row nobody reads. Escape `|` in code as `\\|`,")
        print("  keep tables contiguous, and give every row block a header + delimiter.")
        return 1
    print("markdown tables: %d file(s) clean" % scanned)
    return 0


if __name__ == "__main__":
    sys.exit(main())
