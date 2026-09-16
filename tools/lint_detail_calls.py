#!/usr/bin/env python3
"""No `live_*_calls()` inside a `detail::` body -- the offline-mockability rule, as a machine.

THE RULE. A `detail::` body is what an offline oracle DRIVES. Every callee it reaches must therefore
be substitutable by the test, which is what the `<tu>_calls` struct is for (translator-brief 3b). A
`live_<x>_calls()` evaluated inside such a body defeats that: the table it returns holds
`mh::call::` thunks, each a `__declspec(naked)` jump to an absolute game VA, and the offline
test exe (`libmh_selftest.exe` for every migration domain since fork F5I) loads no game image -- so the case FAULTS before its first assertion rather than failing. The
parent's own mock cannot intercept it, because the sibling is not reached through `c`.

A public wrapper doing the same thing is CORRECT and is 913 of the 919 call sites in this tree: the
wrapper is the boundary where the live tables are supposed to be bound.

WHY A LINT AND NOT A PARAGRAPH -- the brief already had the paragraph. Rule 3b states this exact
failure mode ("unmapped inside the offline test exe ... the case faults instead of failing") and then
exempts an in-manifest SAME-TU sibling, on the reasoning that the parent's mock reaches it. In the
same TU that is true. Across TUs it is not, and the brief neither says where a cross-TU sibling's
table comes from nor carries the same-TU condition into the exemption -- so four writers working in
parallel each reached for `live_<sibling>_calls()`, the only thing in scope. It compiles, it links,
it passes `lint_translation` (which reads the struct to check callee COVERAGE), it passes the build
and every static gate. It is invisible until somebody RUNS a composite offline, which for sim_resid
was three sessions and eight translations later.

MEASURED 2026-08-31 over the whole DLL: 919 call sites in 555 files, 864 direct intra-slice sibling
edges inside `detail` bodies. 786 of those edges call a sibling that takes no table at all; 72
forward the parent's own `c`/`gc`, which is the tree's established shape; 6 hardcoded a `live_*`.
All 6 were in one directory. This lint is what stops the seventh.

REGION DETECTION IS BY THE `namespace detail {` / `} // namespace detail` MARKER PAIR, and every
alternative was tried and was wrong in a way that still produced a plausible number:
  * brace counting -- one depth error early in a file silently flips every later verdict (it
    reported 3 false positives in sim_bldg_turret_combat.cpp, all in the public wrappers);
  * matching the markers on COMMENT-STRIPPED lines -- the closing marker IS a `//` comment, so ~200
    files with a second `namespace mh::x::detail` block at the tail read as unclosed and every
    wrapper in them read as a hit;
  * deleting block comments rather than replacing them with their own newlines -- shifts every
    reported line number after the first `/* */`.
Each of those runs looked like a finding. The check keeps the line-count-preserving strip and the
marker pair, and `--selftest` proves it can still go red.

Usage:
  python tools/lint_detail_calls.py            # the gate: exit 0 iff no detail body binds a live table
  python tools/lint_detail_calls.py --list     # also print the 913 sanctioned wrapper sites, by file
  python tools/lint_detail_calls.py --selftest # prove the gate can go red
"""

import argparse
import glob
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL = os.path.join(REPO, "src", "mh_dll")

CALL = re.compile(r"\blive_([a-z_0-9]+)_calls\s*\(\s*\)")
OPEN = re.compile(r"^\s*namespace\s+(?:[\w:]*::)?detail\s*\{")
CLOSE = re.compile(r"^\s*\}\s*//\s*namespace\s+(?:[\w:]*::)?detail\b")
# the DEFINITION `const X_calls &live_X_calls() {` is not a call site
DEFN = re.compile(r"&\s*live_\w+_calls\s*\(\s*\)\s*\{")
FN = re.compile(r"^[A-Za-z_][\w:<>,*& ]*?\b([a-z_0-9]+)\s*\(")


def strip_comments(src):
    """Line-count preserving: a block comment becomes its own newlines, never nothing."""
    src = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src, flags=re.S)
    return "\n".join(re.sub(r"//.*$", "", ln) for ln in src.split("\n"))


def scan_text(raw, path="<memory>"):
    """-> (hits, unmatched_open_lines). A hit is a dict with `in_detail`."""
    lines = strip_comments(raw).split("\n")
    rawlines = raw.split("\n")
    in_detail, cur, hits, unmatched = False, None, [], []
    for i, line in enumerate(lines):
        # OPEN/CLOSE match the RAW line -- the closing marker is itself a comment.
        rawline = rawlines[i] if i < len(rawlines) else ""
        if OPEN.match(rawline):
            if in_detail:
                unmatched.append(i + 1)
            in_detail, cur = True, None
            continue
        if CLOSE.match(rawline):
            in_detail, cur = False, None
            continue
        fm = FN.match(line)
        if fm and "(" in line and not line.lstrip().startswith("#"):
            cur = fm.group(1)
        for cm in CALL.finditer(line):
            if DEFN.search(rawline):
                continue
            hits.append(
                {
                    "file": path,
                    "line": i + 1,
                    "table": cm.group(1),
                    "in_detail": in_detail,
                    "fn": cur,
                }
            )
    return hits, unmatched


def scan_tree():
    files = sorted(
        set(
            glob.glob(os.path.join(DLL, "**", "*.cpp"), recursive=True)
            + glob.glob(os.path.join(DLL, "**", "*.h"), recursive=True)
        )
    )
    hits, unmatched = [], []
    for f in files:
        raw = open(f, encoding="utf-8", errors="replace").read()
        h, u = scan_text(raw, os.path.relpath(f, REPO).replace("\\", "/"))
        hits += h
        if u:
            unmatched.append((os.path.relpath(f, REPO).replace("\\", "/"), u))
    return hits, unmatched


CLEAN = """
namespace mh::sim {
namespace detail {
void child(const sim_view &v, sim_store &own, const child_calls &c) { c.thing(); }
void parent(const sim_view &v, sim_store &own, const parent_calls &c,
            const child_calls &cc = live_child_calls()) {
    child(v, own, cc);
}
} // namespace detail
void parent(void) { detail::parent(st.read, st.own, live_parent_calls()); }
} // namespace mh::sim
"""

DIRTY = CLEAN.replace("child(v, own, cc);", "child(v, own, live_child_calls());")


def selftest():
    """Prove the gate distinguishes the two, and that the DEFAULT ARGUMENT is not a false positive.

    The clean case deliberately contains a `live_child_calls()` INSIDE the detail region -- as a
    defaulted parameter in the signature, which is the sanctioned Option-B shape: it is evaluated at
    the CALL SITE, so a test that passes its own recorder never reaches it. A checker that flagged
    it would make the fix unimplementable, and one that missed the body call would be useless.
    """
    ok = True
    clean, _ = scan_text(CLEAN)
    dirty, _ = scan_text(DIRTY)
    clean_bad = [h for h in clean if h["in_detail"] and h["fn"] != "parent"]
    dirty_bad = [h for h in dirty if h["in_detail"]]
    # In CLEAN the only in-detail hit is the defaulted parameter on `parent`'s own signature.
    inside_clean = [h for h in clean if h["in_detail"]]
    if len(inside_clean) != 1:
        print(
            "SELFTEST FAIL: clean case has %d in-detail hit(s), expected 1 (the default arg)"
            % len(inside_clean)
        )
        ok = False
    if len(dirty_bad) != 2:
        print("SELFTEST FAIL: dirty case has %d in-detail hit(s), expected 2" % len(dirty_bad))
        ok = False
    if clean_bad:
        print("SELFTEST FAIL: clean case flagged a non-signature site")
        ok = False
    # and the wrapper's own live_parent_calls() must never be in-detail in either
    if any(h["table"] == "parent" and h["in_detail"] for h in clean + dirty):
        print("SELFTEST FAIL: a public wrapper's call read as inside detail")
        ok = False
    print("lint_detail_calls selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def is_default_arg(path, line_no):
    """True if the hit sits in a PARAMETER LIST rather than a statement.

    The sanctioned Option-B shape puts `= live_<sibling>_calls()` in the signature: it is evaluated
    at the caller, so a test that passes its own recorder never touches it. Recognised by the line
    (or the one above it, for a wrapped signature) carrying a `&` parameter declaration and no
    terminating `;`.
    """
    src = open(os.path.join(REPO, path), encoding="utf-8", errors="replace").read().split("\n")
    window = " ".join(src[max(0, line_no - 3) : line_no])
    return bool(re.search(r"\bconst\s+\w+_calls\s*&\s*\w+\s*=\s*live_\w+_calls\s*\(\s*\)", window))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true", help="also print the sanctioned wrapper sites")
    ap.add_argument("--selftest", action="store_true", help="prove the gate can go red")
    args = ap.parse_args()
    if args.selftest:
        return selftest()

    hits, unmatched = scan_tree()
    rc = 0
    for f, lines in unmatched:
        print("  !! nested/unclosed `namespace detail` in %s at line(s) %s" % (f, lines))
        rc = 1

    inside = [h for h in hits if h["in_detail"]]
    bad = [h for h in inside if not is_default_arg(h["file"], h["line"])]
    outside = [h for h in hits if not h["in_detail"]]

    if bad:
        print(
            "  %d `live_*_calls()` inside a `detail` body -- an offline oracle FAULTS on these "
            "before its first assertion:" % len(bad)
        )
        for h in sorted(bad, key=lambda x: (x["file"], x["line"])):
            print(
                "    %s:%d  in %s()  ->  live_%s_calls()"
                % (h["file"], h["line"], h["fn"] or "?", h["table"])
            )
        print(
            "  FIX: give the caller a defaulted parameter -- "
            "`const <sib>_calls &c_<sib> = live_<sib>_calls()` -- and forward it at the call site. "
            "The default IS today's behaviour, so nothing changes except that a test can substitute "
            "it. See translator-brief 3b."
        )
        rc = 1
    else:
        print(
            "  no `live_*_calls()` in a `detail` body (%d call site(s) in %d file(s); "
            "%d sanctioned wrapper site(s), %d defaulted parameter(s))"
            % (len(hits), len({h["file"] for h in hits}), len(outside), len(inside))
        )
    if args.list:
        for h in sorted(outside, key=lambda x: (x["file"], x["line"])):
            print("    ok %s:%d  live_%s_calls()" % (h["file"], h["line"], h["table"]))
    return rc


if __name__ == "__main__":
    sys.exit(main())
