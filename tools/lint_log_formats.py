#!/usr/bin/env python3
"""lint_log_formats -- the LOG LINE FORMAT registry gate (mp:SES2).

WHY THIS EXISTS. the instrument-channels doc measured the state this replaces: ~65 distinct parsed
line formats across 11 consumer tools, with **zero** shared registry -- every parser hard-coding its
own needle, and nothing at all connecting a needle to the C++ that emits it. The failure mode is not
hypothetical and the inventory that built `tools/data/log_formats.json` found three live instances of
it in one afternoon:

  * `mp_pacing_report.read_frametimes` still keys on a `qpc_freq=` header token the DLL stopped
    emitting under D22. It does not error -- `if prev is not None and freq:` short-circuits, so every
    frame-time column in the pacing report has been silently `nan`/0 on every current log since.
  * `mp_analyze`'s unplanned-end marker list still carries `"kicked-off the game"`, which no longer
    appears anywhere in `src/mh_dll`.
  * `test_ui`'s `INSTALL_REFUSALS` records, in a comment, a needle that had to be special-cased
    because reusing another line's text matched the wrong line.

A registry alone would have caught none of those, because a registry nobody checks is a fourth copy
of the list. So this file is the check, and it has three arms.

THE MATCHING RULE, kept deliberately small so it can be stated in one paragraph:

  ARM A -- STALE ENTRY (registry -> parser). For every `formats[].parsers` row `tools/x.py:fn`, at  # CITATION-OK (fixture/example path, not a repo citation)
    least one of that format's `needles` must appear VERBATIM in `tools/x.py`. A needle is compared  # CITATION-OK (fixture/example path, not a repo citation)
    against the tool's string literals first (read with `ast`, so comments and docstrings are out of
    scope by construction) and against the raw source as a fallback, which covers a needle assembled
    by concatenation. A parser that stops reading a format must drop the row.

  ARM B -- UNREGISTERED PARSE (parser -> registry). Every registered parser tool is scanned for
    CHANNEL TAGS: a string literal containing `;` followed by `[tag]` (the seam-line prefix every
    `; [promote]` / `; [interlock]` / `; [session]` line carries), with regex backslash-escapes
    normalised away so `r"; \\[script\\] SIGNAL"` reads as `; [script]`. Each tag found must be
    carried by at least one registry entry that names THAT tool as a parser. This is the arm that
    goes red when a tool starts parsing a format the registry does not list.

  ARM C -- STALE EMITTER (registry -> C++). Every format with an `emitter` must name a file that
    exists under `src/`, and that file must still contain the format's `emit` literal -- the
    distinctive fragment of the C++ format string. This is the arm that catches the `qpc_freq=`
    class: a parser and a registry that agree with each other about a line the emitter stopped
    writing.

WHAT ARM B DOES NOT CLAIM, stated because the honest scope is narrower than "every format":
`; [tag]`-prefixed lines are mechanically extractable and column-header streams, `net: ` lines and
free-form structural lines are not -- a needle like `"GAMEOVER survivor="` cannot be told apart from
any other string a tool holds. Those formats are REGISTERED (arms A and C cover them in full) but
they are not discovered by arm B, so a tool that starts parsing a brand-new free-form line is not
red until someone registers it. The bracketed class was chosen because it is the one with a
syntactic marker, not because it is the whole population. Widening arm B means giving the other
classes a marker of their own, which is a change to the emitters, not to this file.

  python tools/lint_log_formats.py              # the gate (three arms over the real tree)
  python tools/lint_log_formats.py --selftest   # plant each failure and require it to go red
  python tools/lint_log_formats.py --report     # the inventory, by stream
"""

import argparse
import ast
import json
import os
import re
import shutil
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGISTRY = os.path.join(REPO, "tools", "data", "log_formats.json")

# A CHANNEL TAG as it appears in a parser's string literal, after backslash normalisation:
#   "; [promote] "        -> promote
#   r"; \[script\] SIGNAL" -> script   (the backslashes are stripped first)
# The leading `;` is REQUIRED. Without it, `[a-z]`-shaped character classes in ordinary regexes
# would be read as tags and this lint would red on innocent code.
TAG_RE = re.compile(r";\s*\[([a-z][a-z0-9_]{1,23})\]")


def norm(text):
    """Regex-escape normalisation: drop backslashes, collapse runs of whitespace.

    `r"; \\[script\\]"` and `"; [script]"` are the same channel tag written two ways, and a registry
    that had to list both spellings would be a registry of spellings rather than of formats.
    """
    return re.sub(r"\s+", " ", text.replace("\\", ""))


def literals_of(path):
    """Every string literal the MODULE'S CODE uses, as a list -- comments and docstrings excluded.

    Read through `ast` rather than by regex: comments never enter the tree at all, and docstrings are
    identified as such and dropped. That is the whole of the exclusion rule, and it is why a tool may
    discuss a channel it does not parse in prose without this lint demanding a registry row for it.
    """
    with open(path, encoding="utf-8") as fh:
        src = fh.read()
    tree = ast.parse(src, filename=path)
    docstrings = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
            body = getattr(node, "body", None)
            if (
                body
                and isinstance(body[0], ast.Expr)
                and isinstance(body[0].value, ast.Constant)
                and isinstance(body[0].value.value, str)
            ):
                docstrings.add(id(body[0].value))
    out = []
    for node in ast.walk(tree):
        if (
            isinstance(node, ast.Constant)
            and isinstance(node.value, str)
            and id(node) not in docstrings
        ):
            out.append(node.value)
    return out, src


def _no_duplicate_keys(pairs):
    """object_pairs_hook: a repeated key inside one object is a merge accident, not data.

    Two format objects textually concatenated with a missing ``},{`` parse as ONE object whose
    later ``id`` silently wins, and the earlier format disappears on the next json round trip
    (``net.bulk_lane_rollup`` was lost that way during wave 9, 2026-09-18). Fail loudly instead.
    """
    seen = {}
    for k, v in pairs:
        if k in seen:
            raise ValueError(
                "duplicate key %r in one object (ids %r / %r) -- two formats concatenated "
                "without a '},{' between them" % (k, seen.get("id"), v if k == "id" else "?")
            )
        seen[k] = v
    return seen


def load(registry_path=REGISTRY):
    with open(registry_path, encoding="utf-8") as fh:
        return json.load(fh, object_pairs_hook=_no_duplicate_keys)


def check(registry_path=REGISTRY, repo=REPO):
    reg = load(registry_path)
    formats = reg["formats"]
    fails = []

    ids = [f["id"] for f in formats]
    if len(ids) != len(set(ids)):
        dup = sorted({i for i in ids if ids.count(i) > 1})
        fails.append("the registry lists an id twice: %s" % ", ".join(dup))

    streams = set(reg.get("streams", {}))
    parser_tools = {}  # tools/x.py -> (literals, raw source)  # CITATION-OK (fixture/example path, not a repo citation)
    tool_tags = {}  # tools/x.py -> {tag}  # CITATION-OK (fixture/example path, not a repo citation)
    entry_tools = {}  # tools/x.py -> {tag registered for it}  # CITATION-OK (fixture/example path, not a repo citation)

    for f in formats:
        if streams and f["stream"] not in streams:
            fails.append(
                "%s names stream %r, which the registry's `streams` vocabulary does not carry"
                % (f["id"], f["stream"])
            )
        if not f.get("needles"):
            fails.append(
                "%s has no needles -- a format nothing can be matched by is not a format" % f["id"]
            )
        for p in f["parsers"]:
            tool = p.split(":", 1)[0]
            path = os.path.join(repo, tool.replace("/", os.sep))
            if not os.path.isfile(path):
                fails.append("%s names parser %s, and %s does not exist" % (f["id"], p, tool))
                continue
            if tool not in parser_tools:
                try:
                    parser_tools[tool] = literals_of(path)
                except SyntaxError as exc:
                    fails.append("%s does not parse as Python: %s" % (tool, exc))
                    parser_tools[tool] = ([], "")
                lits, src = parser_tools[tool]
                tool_tags[tool] = {m for lit in lits for m in TAG_RE.findall(norm(lit))}
            entry_tools.setdefault(tool, set()).update(f.get("tags", []))

            # ---- ARM A: the registry's claim about this parser must still be true ----------------
            lits, src = parser_tools[tool]
            blob = "\n".join(lits)
            if not any((n in blob) or (n in src) for n in f["needles"]):
                fails.append(
                    "STALE ENTRY: %s claims %s parses it, but none of its needles appear there "
                    "(%s)" % (f["id"], p, ", ".join(repr(n) for n in f["needles"][:3]))
                )

        # ---- ARM C: the emitter must still write it ------------------------------------------------
        emitter = f.get("emitter")
        emit = f.get("emit")
        if emitter:
            epath = os.path.join(repo, emitter.split(":", 1)[0].replace("/", os.sep))
            if not os.path.isfile(epath):
                fails.append("%s names emitter %s, which does not exist" % (f["id"], emitter))
            elif not emit:
                fails.append(
                    "%s names an emitter but no `emit` literal to look for in it" % f["id"]
                )
            else:
                with open(epath, encoding="utf-8", errors="replace") as fh:
                    csrc = fh.read()
                if emit not in csrc:
                    fails.append(
                        "STALE EMITTER: %s expects %r in %s, and it is not there any more"
                        % (f["id"], emit, emitter)
                    )
        elif emit:
            fails.append(
                "%s carries an `emit` literal but names no emitter to look for it in" % f["id"]
            )

    # ---- ARM B: every channel tag a registered parser uses must be a registered format -------------
    for tool, tags in sorted(tool_tags.items()):
        for tag in sorted(tags):
            if tag not in entry_tools.get(tool, set()):
                fails.append(
                    "UNREGISTERED PARSE: %s matches the channel tag '; [%s]' and no registry entry "
                    "lists that tag with %s as a parser -- add it to tools/data/log_formats.json"
                    % (tool, tag, tool)
                )

    # ---- non-vacuity. A registry that got emptied, or a scan that found no tags at all, must not
    # read as a clean pass: both are exactly what a broken run looks like.
    if not formats:
        fails.append("the registry is EMPTY -- a vacuous pass, not a clean one")
    if not any(tool_tags.values()):
        fails.append(
            "no channel tag was found in ANY registered parser -- arm B found nothing to check, "
            "which is a broken scan, not a clean tree"
        )
    return fails, reg, tool_tags


def report(reg):
    by_stream = {}
    for f in reg["formats"]:
        by_stream.setdefault(f["stream"], []).append(f)
    print(
        "LOG LINE FORMAT REGISTRY -- %d format(s) across %d stream(s)"
        % (len(reg["formats"]), len(by_stream))
    )
    tools = sorted({p.split(":", 1)[0] for f in reg["formats"] for p in f["parsers"]})
    print(
        "  %d parser tool(s): %s" % (len(tools), ", ".join(t.replace("tools/", "") for t in tools))
    )
    emitted = sum(1 for f in reg["formats"] if f.get("emitter"))
    print(
        "  %d format(s) carry a verified emitter; %d do not"
        % (emitted, len(reg["formats"]) - emitted)
    )
    for stream in sorted(by_stream):
        rows = by_stream[stream]
        print("\n  %s -- %d format(s)" % (stream, len(rows)))
        for f in sorted(rows, key=lambda r: r["id"]):
            print(
                "    %-38s %-46s %s"
                % (f["id"], f.get("emitter") or "(emitter not pinned)", ",".join(f["parsers"]))
            )


# ---- the selftest: plant each failure and require it to be caught --------------------------------
#
# Rendered from the REAL registry rather than from a hand-written fixture, so an empty or broken
# registry cannot make the selftest pass vacuously -- the clean arm asserts the render is non-vacuous
# before any planting happens.
CLEAN_TOOL = '''"""A docstring that mentions ; [ghost] and must NOT be read as a parse."""
# a comment that mentions ; [phantom] and must NOT be read as a parse either
import re

MARK = "; [alpha] armed"
PAT = re.compile(r"; \\[beta\\] count=(\\d+)")


def read(text):
    return MARK in text, PAT.search(text)
'''

CLEAN_EMITTER = """
void arm(void) {
    say("; [alpha] armed\\n");
    say("; [beta] count=%d\\n", n);
}
"""


def _render(tmp):
    os.makedirs(os.path.join(tmp, "tools", "data"))
    os.makedirs(os.path.join(tmp, "src", "x"))
    with open(os.path.join(tmp, "tools", "probe.py"), "w", encoding="utf-8") as fh:
        fh.write(CLEAN_TOOL)
    with open(os.path.join(tmp, "src", "x", "emit.cpp"), "w", encoding="utf-8") as fh:
        fh.write(CLEAN_EMITTER)
    reg = {
        "streams": {"mh_probe.log": "the fixture stream"},
        "formats": [
            {
                "id": "probe.alpha",
                "stream": "mh_probe.log",
                "emitter": "src/x/emit.cpp",  # CITATION-OK
                "emit": "; [alpha] armed",
                "tags": ["alpha"],
                "needles": ["; [alpha] armed"],
                "fields": [],
                "parsers": ["tools/probe.py:read"],  # CITATION-OK
                "since": "fixture",
            },
            {
                "id": "probe.beta",
                "stream": "mh_probe.log",
                "emitter": "src/x/emit.cpp",  # CITATION-OK
                "emit": "; [beta] count=",
                "tags": ["beta"],
                "needles": ["; \\[beta\\] count=(\\d+)"],
                "fields": ["count"],
                "parsers": ["tools/probe.py:read"],  # CITATION-OK
                "since": "fixture",
            },
        ],
    }
    path = os.path.join(tmp, "tools", "data", "log_formats.json")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(reg, fh, indent=2)
    return path


def selftest():
    fails = 0

    def arm(name, ok):
        nonlocal fails
        if ok:
            print("  ok: %s" % name)
        else:
            fails += 1
            print("  [FAIL] %s" % name)

    tmp = tempfile.mkdtemp(prefix="lint_log_formats_")
    try:
        path = _render(tmp)
        base, reg, tags = check(path, tmp)
        arm("the clean fixture passes", not base)
        arm(
            "...and it is NON-VACUOUS (2 formats, both tags actually found in the tool)",
            len(reg["formats"]) == 2
            and tags.get("tools/probe.py") == {"alpha", "beta"},  # CITATION-OK
        )
        arm(
            "a tag mentioned only in a COMMENT or a DOCSTRING is not read as a parse",
            "phantom" not in tags.get("tools/probe.py", set())  # CITATION-OK
            and "ghost" not in tags.get("tools/probe.py", set()),  # CITATION-OK
        )

        # ARM B: a parser starts matching a channel the registry does not list.
        with open(os.path.join(tmp, "tools", "probe.py"), "a", encoding="utf-8") as fh:
            fh.write('\nPLANTED = "; [gamma] surprise"\n')
        f2, _, _ = check(path, tmp)
        arm(
            "a PLANTED unregistered channel tag goes RED, naming the tag and the tool",
            any("UNREGISTERED PARSE" in m and "gamma" in m and "probe.py" in m for m in f2),
        )
        # And the same planting inside a COMMENT must NOT go red -- otherwise the arm is a grep and
        # the exclusion rule above is decoration.
        with open(os.path.join(tmp, "tools", "probe.py"), "w", encoding="utf-8") as fh:
            fh.write(CLEAN_TOOL + '\n# PLANTED IN A COMMENT: "; [gamma] surprise"\n')
        f2b, _, _ = check(path, tmp)
        arm("...but the same text planted in a COMMENT does not", not f2b)

        # ARM A: the parser stops reading a registered format.
        with open(os.path.join(tmp, "tools", "probe.py"), "w", encoding="utf-8") as fh:
            fh.write(CLEAN_TOOL.replace('"; [alpha] armed"', '"; [alpha] rearmed"'))
        f3, _, _ = check(path, tmp)
        arm(
            "a needle the parser no longer carries goes RED as a STALE ENTRY",
            any("STALE ENTRY" in m and "probe.alpha" in m for m in f3),
        )

        # ARM C: the emitter stops writing a registered format.
        with open(os.path.join(tmp, "tools", "probe.py"), "w", encoding="utf-8") as fh:
            fh.write(CLEAN_TOOL)
        with open(os.path.join(tmp, "src", "x", "emit.cpp"), "w", encoding="utf-8") as fh:
            fh.write(CLEAN_EMITTER.replace("; [beta] count=", "; [beta] total="))
        f4, _, _ = check(path, tmp)
        arm(
            "an emit literal the C++ no longer writes goes RED as a STALE EMITTER",
            any("STALE EMITTER" in m and "probe.beta" in m for m in f4),
        )

        # The two structural refusals: a named file that is not there at all.
        with open(os.path.join(tmp, "src", "x", "emit.cpp"), "w", encoding="utf-8") as fh:
            fh.write(CLEAN_EMITTER)
        os.remove(os.path.join(tmp, "tools", "probe.py"))
        f5, _, _ = check(path, tmp)
        arm("a parser file that does not exist goes RED", any("does not exist" in m for m in f5))

        with open(os.path.join(tmp, "tools", "probe.py"), "w", encoding="utf-8") as fh:
            fh.write(CLEAN_TOOL)
        os.remove(os.path.join(tmp, "src", "x", "emit.cpp"))
        f6, _, _ = check(path, tmp)
        arm(
            "an emitter file that does not exist goes RED",
            any("emitter" in m and "does not exist" in m for m in f6),
        )

        # An EMPTIED registry must not read as clean. This is the arm that keeps the gate from
        # degrading into "nothing to check, therefore fine".
        with open(os.path.join(tmp, "src", "x", "emit.cpp"), "w", encoding="utf-8") as fh:
            fh.write(CLEAN_EMITTER)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump({"streams": {}, "formats": []}, fh)
        f7, _, _ = check(path, tmp)
        arm("an EMPTY registry is a failure, not a vacuous pass", any("EMPTY" in m for m in f7))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("=== lint_log_formats --selftest: %d failure(s) ===" % fails)
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--selftest", action="store_true", help="plant each failure and require it to go red"
    )
    ap.add_argument("--report", action="store_true", help="print the inventory, by stream")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    fails, reg, tool_tags = check()
    if args.report:
        report(reg)
        print("")
    if fails:
        for m in fails:
            print("  [FAIL] %s" % m)
        print(
            "lint_log_formats: %d problem(s). The registry is tools/data/log_formats.json; "
            "the instrument-channels doc carries what it is for." % len(fails)
        )
        return 1
    tools = sorted({p.split(":", 1)[0] for f in reg["formats"] for p in f["parsers"]})
    print(
        "lint_log_formats: ok -- %d format(s), %d stream(s), %d parser tool(s), %d channel tag(s) "
        "checked"
        % (
            len(reg["formats"]),
            len({f["stream"] for f in reg["formats"]}),
            len(tools),
            sum(len(t) for t in tool_tags.values()),
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
