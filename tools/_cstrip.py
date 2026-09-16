#!/usr/bin/env python3
"""_cstrip.py -- ONE correct C/C++ comment stripper, because six tools had the same bug.

WHAT WAS WRONG. Every scanner in tools/ that wanted "the code without the prose" wrote the same two
substitutions, in this order:

    text = re.sub(r"/\\*.*?\\*/", "", text, flags=re.S)   # block comments first
    text = re.sub(r"//[^\\n]*", "", text)                 # then line comments

That is order-dependent and the order is backwards. A `/*` INSIDE a `//` comment opens a block
comment as far as the first regex is concerned, and it stays open until the next `*/` ANYWHERE in the
file -- which in this tree is typically an inline `/*param=*/` annotation dozens of lines later.
Everything between is deleted from the scanner's view.

MEASURED, 2026-09-05, over the 1281 files under src/mh_dll/mh: 8 files lose more than 40 characters
and 21,199 characters vanish in total. The four worst are all sim TUs and the losses are 2.3-6.3 KB
each:

    6276  sim/sim_unit_state_deploy.cpp
    6269  sim/sim_map_region_tile_find.cpp
    4894  sim/sim_bldg_state_charge.cpp
    2306  sim/sim_bldg_state_dismantle.cpp

The trigger is idiomatic here, which is why it went unnoticed for so long: a header banner cites the
Ghidra draft it was translated from -- `// ... (tmp/decomp/<fn>_<addr>.c) -- both agree with the
assembly` -- and `/*.c)` is a block-comment opener. The `*/` that closes it is an ordinary
`/*hard_remove=*/1` argument annotation further down. Between them, in sim_bldg_state_dismantle.cpp,
sat an ENTIRE calls struct binding nine members to `mh::call::` -- invisible to
gen_liveness_census.py, which therefore reported the row it bound as live-by-some-other-route and
never counted the binding SIM1-P clause 2 exists to remove.

The failure direction is the dangerous one: a scanner that cannot see a construct reports its ABSENCE,
and absence is what these tools are usually asked to certify.

SO: one scanner, left to right, in a single pass, that also understands string and character
literals (a `"//"` in a path or a format string is not a comment). Anything in tools/ that needs
comment-free source imports this rather than writing the pair again.

    from _cstrip import strip_comments      # same-directory import, as tools/_reimpl.py is used
    code = strip_comments(open(path).read())

`keep_lines=True` replaces each removed comment with an equal number of newlines, so line numbers in
the output still match the file -- what a linter that reports positions needs.

Self-test: `python tools/_cstrip.py` runs the cases below and prints PASS/FAIL. They are the real
shapes from this tree, not invented ones.
"""

from __future__ import annotations


def strip_comments(text: str, keep_lines: bool = False) -> str:
    """C/C++ comments removed in ONE left-to-right pass; string and char literals preserved."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        # A literal is scanned whole, so a `//` or `/*` inside it is never a comment.
        if c == '"' or c == "'":
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\":  # any escape, including \" and \\
                    out.append(text[i : i + 2])
                    i += 2
                    continue
                out.append(text[i])
                closed = text[i] == c
                i += 1
                if closed:
                    break
            continue
        if c == "/" and i + 1 < n:
            if text[i + 1] == "/":
                j = text.find("\n", i)
                i = n if j < 0 else j  # keep the newline itself
                continue
            if text[i + 1] == "*":
                j = text.find("*/", i + 2)
                end = n if j < 0 else j + 2
                if keep_lines:
                    out.append("\n" * text.count("\n", i, end))
                i = end
                continue
        out.append(c)
        i += 1
    return "".join(out)


def _self_test() -> int:
    cases = [
        # THE BUG. A `//` banner citing a draft file, then an inline annotation far below. The old
        # pair deleted everything between; the binding in the middle must survive.
        (
            "// from (tmp/decomp/f_0049.c) -- agrees\n"
            "  mh::call::llm_strat_bldg_refund,\n"
            "  g(x, /*hard_remove=*/1);\n",
            "mh::call::llm_strat_bldg_refund",
            True,
        ),
        # An ordinary block comment really is removed.
        ("a /* gone */ b", "gone", False),
        # An ordinary line comment really is removed.
        ("a // gone\nb", "gone", False),
        # `//` inside a string literal is not a comment.
        ('const char *u = "http://x"; int keep;', "http://x", True),
        # `/*` inside a string literal does not open a comment.
        ('const char *s = "/*"; int keep_me;', "keep_me", True),
        # An escaped quote does not end the literal.
        ('const char *s = "a\\"//b"; int keep_me;', "keep_me", True),
        # A char literal holding a quote.
        ("char q = '\"'; int keep_me;", "keep_me", True),
        # Unterminated block comment eats the rest, as the compiler would.
        ("int a; /* open\nint gone;", "gone", False),
    ]
    bad = 0
    for src, token, want in cases:
        got = token in strip_comments(src)
        if got != want:
            bad += 1
            print(f"FAIL: {token!r} present={got} want={want} in {src!r}")
    # keep_lines preserves the line count exactly.
    src = "a\n/* x\ny\nz */\nb\n"
    if strip_comments(src, keep_lines=True).count("\n") != src.count("\n"):
        bad += 1
        print("FAIL: keep_lines changed the line count")
    print("PASS" if bad == 0 else f"{bad} FAILURE(S)")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(_self_test())
