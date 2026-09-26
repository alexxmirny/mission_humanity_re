#!/usr/bin/env python3
"""lint_shutdown_counters.py -- tooling:TL-SUITE-COUNTERS. Refuse a counter reported only at teardown.

Dead-end G101: the transport's `g_dropped` counted the exact loss D24 spent three sessions
hunting, and no artifact of any run ever showed it, because its only print was one line inside the
transport's graceful-shutdown path --

    logf("net: shutdown (dropped %ld queued msgs over run)", g_dropped);

-- and every rig determinism run ends by KILLING the peers, never by an exit that reaches that line.
"The number exists in a struct" is not instrumentation in a system whose runs are never allowed to
exit cleanly. mh_common/include/mh_diag_counter.h is the shared fix (a first-hit line plus a periodic
rollup from a cadence that already runs); this lint is the gate that keeps a NEW counter from
reintroducing the same shape.

WHAT IS SCANNED: every `g_<name>` global/static counter (this project's own naming convention for
exactly this kind of state -- see the dozens already in mh_net/net_transport.cpp, mh/desync/
desync_watch.cpp, mh_harness/harness.cpp) declared at file scope in src/mh_dll/{mh,mh_common,
mh_harness,libmh,mh_net,mh_net_udp} with an integral type, that is INCREMENTED somewhere in its file
(`++g_x`, `g_x++`, `g_x +=`, `InterlockedIncrement(&g_x)`, `InterlockedExchangeAdd(&g_x, ...)`) and
PRINTED somewhere in its file, through one of this codebase's own log sinks (logf/say/log_line/
append_line/append_line_once/net_log_line/seam_log/OutputDebugStringA -- every module has its own,
there is no single global one).

A counter is an OFFENDER when it has at least one increment site, at least one print site, and EVERY
print site is a "shutdown-path" reference: the print call's own enclosing function name matches
SHUTDOWN_NAME_RE (shutdown/teardown/_fini/_close/process_detach/atexit/on_exit), OR the call's own
joined argument text matches SHUTDOWN_TEXT_RE (the message says "shutdown"/"teardown"/"at exit"/"on
exit"/"over run"/"over the run"/"final tally"). A counter with even ONE print site outside that set
(a first-hit guard, a periodic rollup, a per-event line, ...) is not flagged -- it is already visible
under a kill.

THIS IS A HEURISTIC OVER TEXT, deliberately (this project's other src-side lints -- lint_resources,
lint_ui_sync -- are plain regex/line scans too, not a real C++ parse; see their own docstrings for
why). Two known blind spots, both narrower than what they replace (a lint that never ran at all):
  * a `case DLL_PROCESS_DETACH:` arm inside DllMain is not resolved to a synthetic "function name",
    so a counter printed only there, with a message carrying none of the SHUTDOWN_TEXT_RE words, would
    not be caught by name; every DLL_PROCESS_DETACH arm in this tree today is documented as
    deliberately empty of exactly this kind of work (mh.c's detach arm; MH_CrashMarker_Shutdown has no
    counters). If one grows a counter print, name the counter's report function `..._shutdown` or say
    "shutdown" in the line and this lint catches it the ordinary way.
  * multi-line function signatures whose opening `{` is not on the signature line itself, nor alone on
    the very next line, will not be recognised as a function boundary; the print call then attributes
    to the ENCLOSING scope instead (usually still correct, since C++ in this tree keeps one function
    per named scope).
Both are documented rather than silently "handled" -- an allow-list entry is the correct tool for a
proven case this misses, not a parser rewrite for a gate this cheap.

A flagged counter is accepted onto tools/data/shutdown_counter_allow.json (path + counter + a real
reason + the tracker id that explains it), the same shape as lint_ui_sync's allow-list.

Usage:
    python tools/lint_shutdown_counters.py             # scan src/mh_dll, refuse un-allow-listed hits
    python tools/lint_shutdown_counters.py --selftest  # planted positive + negative, prove both fire
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN_DIRS = ["mh", "mh_common", "mh_harness", "libmh", "mh_net", "mh_net_udp"]
ALLOW_PATH = os.path.join(REPO, "tools", "data", "shutdown_counter_allow.json")

LOG_CALL_RE = re.compile(
    r"\b(logf|say|log_line|append_line|append_line_once|net_log_line|seam_log|OutputDebugStringA)\s*\("
)
DECL_RE = re.compile(
    r"^\s*(?:static\s+)?(?:volatile\s+)?"
    r"(?:long|int|unsigned(?:\s+(?:long|int))?|uint32_t|uint64_t|size_t|LONG|DWORD)\s+"
    r"(g_[A-Za-z0-9_]+)\s*(?:=\s*[^;]*)?;"
)
INCR_RES = [
    re.compile(r"\+\+\s*(g_[A-Za-z0-9_]+)\b"),
    re.compile(r"\b(g_[A-Za-z0-9_]+)\s*\+\+"),
    re.compile(r"\b(g_[A-Za-z0-9_]+)\s*\+=\s*[\w(]"),
    re.compile(
        r"Interlocked(?:Increment|ExchangeAdd)\w*\s*\(\s*(?:\([^()]*\)\s*)?&\s*(g_[A-Za-z0-9_]+)\b"
    ),
]
CONTROL_KEYWORDS = {"if", "for", "while", "switch", "catch", "else", "do", "return"}
# a same-line "name(args) {" function-open
FUNC_BRACE_RE = re.compile(
    r"^\s*[\w:<>\*&,\s~]*?\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*(?:const\s*)?(?:noexcept\s*)?\{\s*(?://.*)?$"
)
# a signature line ending in `)` with the `{` on its own next line
FUNC_NOBRACE_RE = re.compile(
    r"^\s*[\w:<>\*&,\s~]*?\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*(?:const\s*)?(?:noexcept\s*)?\s*$"
)
SHUTDOWN_NAME_RE = re.compile(
    r"(shutdown|teardown|_fini\b|_close\b|process_detach|atexit|on_exit)", re.IGNORECASE
)
SHUTDOWN_TEXT_RE = re.compile(
    r"(shutdown|teardown|\bat exit\b|\bon exit\b|\bover run\b|\bover the run\b|final tally)",
    re.IGNORECASE,
)


def _iter_files(root=None):
    root = root or REPO
    for d in SCAN_DIRS:
        for path in sorted(
            glob.glob(os.path.join(root, "src", "mh_dll", d, "**", "*.cpp"), recursive=True)
        ):
            if ".gen." in os.path.basename(path):
                continue
            yield path


def _func_scopes(lines):
    """[(line_index, enclosing_function_name_or_None)] for every line, via a brace-depth heuristic
    (see module docstring for its two known blind spots)."""
    depth = 0
    stack = []  # [(name, depth_at_open)]
    pending = None  # a name detected on a signature line whose `{` is on the NEXT line
    out = []
    for line in lines:
        opener = None
        if pending is not None and line.strip() == "{":
            opener = pending
            pending = None
        else:
            m = FUNC_BRACE_RE.match(line)
            if m and m.group(1) not in CONTROL_KEYWORDS:
                opener = m.group(1)
            else:
                m2 = FUNC_NOBRACE_RE.match(line)
                if m2 and m2.group(1) not in CONTROL_KEYWORDS:
                    pending = m2.group(1)
        for ch in line:
            if ch == "{":
                depth += 1
                if opener is not None:
                    stack.append((opener, depth))
                    opener = None
            elif ch == "}":
                if stack and stack[-1][1] == depth:
                    stack.pop()
                depth = max(0, depth - 1)
        out.append(stack[-1][0] if stack else None)
    return out


def _joined_call(lines, start_idx, max_extra=15):
    """The text of the call statement starting at lines[start_idx], joined forward until its
    parentheses balance (bounded). Best-effort -- see module docstring."""
    text = lines[start_idx]
    depth = text.count("(") - text.count(")")
    i = start_idx
    while depth > 0 and i - start_idx < max_extra and i + 1 < len(lines):
        i += 1
        text += "\n" + lines[i]
        depth += lines[i].count("(") - lines[i].count(")")
    return text


def scan_file(path, lines=None):
    """[(counter, incr_lineno, [print_linenos], all_shutdown, reasons)] for every counter in `path`
    that has both an increment and a print site. `lines` lets the selftest pass a synthetic body."""
    if lines is None:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.read().splitlines()

    declared = set()
    for line in lines:
        m = DECL_RE.match(line)
        if m:
            declared.add(m.group(1))
    if not declared:
        return []

    incr_lines = {}
    for idx, line in enumerate(lines):
        for rx in INCR_RES:
            for m in rx.finditer(line):
                name = m.group(1)
                if name in declared:
                    incr_lines.setdefault(name, idx)

    scopes = _func_scopes(lines)

    print_sites = {}  # counter -> [(line_idx, is_shutdown)]
    for idx, line in enumerate(lines):
        if not LOG_CALL_RE.search(line):
            continue
        call_text = _joined_call(lines, idx)
        for name in declared:
            if re.search(r"\b" + re.escape(name) + r"\b", call_text):
                fn = scopes[idx]
                is_shutdown = bool(fn and SHUTDOWN_NAME_RE.search(fn)) or bool(
                    SHUTDOWN_TEXT_RE.search(call_text)
                )
                print_sites.setdefault(name, []).append((idx, is_shutdown))

    results = []
    for name, incr_idx in incr_lines.items():
        sites = print_sites.get(name)
        if not sites:
            continue  # incremented but never printed at all -- a different lint's problem, not ours
        all_shutdown = all(s for _, s in sites)
        if all_shutdown:
            results.append((name, incr_idx + 1, [i + 1 for i, _ in sites], True))
    return results


def load_allow(path=ALLOW_PATH):
    if not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def allow_problems(entries):
    problems = []
    seen = set()
    for e in entries:
        if not isinstance(e, dict):
            problems.append("entry is not an object: %r" % (e,))
            continue
        missing = [k for k in ("path", "counter", "reason", "tracker_id") if not e.get(k)]
        if missing:
            problems.append("entry %r missing field(s): %s" % (e, ", ".join(missing)))
            continue
        key = (e["path"], e["counter"])
        if key in seen:
            problems.append("duplicate allow-list entry: %s:%s" % key)
        seen.add(key)
    return problems


def run(root=None):
    """{rel_path: [(counter, incr_line, [print_lines])]} of every un-allow-listed offender."""
    allow = load_allow()
    allowed = {(e["path"], e["counter"]) for e in allow if isinstance(e, dict) and e.get("path")}
    offenders = {}
    for path in _iter_files(root):
        rel = os.path.relpath(path, root or REPO).replace(os.sep, "/")
        hits = scan_file(path)
        kept = [(n, il, pl) for (n, il, pl, _) in hits if (rel, n) not in allowed]
        if kept:
            offenders[rel] = kept
    return offenders


# ---- selftest ---------------------------------------------------------------------------------------


def selftest():
    ok = True

    def check(label, cond, detail=""):
        nonlocal ok
        print(
            "  %s  %s%s"
            % ("ok  " if cond else "FAIL", label, ("  -- %s" % detail) if detail else "")
        )
        ok = ok and cond

    # planted POSITIVE: a counter incremented in normal flow, printed ONLY inside a *_shutdown() body.
    bad = [
        "long g_lost = 0;",
        "",
        "void on_frame() {",
        "    ++g_lost;",
        "}",
        "",
        "void conn_shutdown() {",
        '    logf("net: shutdown (lost %ld)", g_lost);',
        "}",
    ]
    hits = scan_file("synthetic_bad.cpp", lines=bad)
    check(
        "a counter printed only inside a *_shutdown() body is flagged",
        any(n == "g_lost" for n, _, _, shut in hits if shut),
    )

    # planted POSITIVE via message text alone (function name gives no hint).
    bad2 = [
        "int g_evicted = 0;",
        "",
        "void reset() {",
        "    ++g_evicted;",
        "}",
        "",
        "void report() {",
        '    logf("total %d evicted over the run", g_evicted);',
        "}",
    ]
    hits2 = scan_file("synthetic_bad2.cpp", lines=bad2)
    check(
        "a counter whose only print's MESSAGE says 'over the run' is flagged even with a plain "
        "function name",
        any(n == "g_evicted" for n, _, _, shut in hits2 if shut),
    )

    # planted NEGATIVE: first-hit + periodic rollup (the fixed shape), same counter name reused.
    good = [
        "long g_lost = 0;",
        "",
        "void on_frame() {",
        "    ++g_lost;",
        '    if (g_lost == 1) logf("net: first loss observed");',
        "}",
        "",
        "void periodic_tick() {",
        '    logf("net: rollup lost=%ld", g_lost);',
        "}",
    ]
    hits3 = scan_file("synthetic_good.cpp", lines=good)
    check(
        "first-hit + periodic rollup is NOT flagged", not any(n == "g_lost" for n, _, _, _ in hits3)
    )

    # planted NEGATIVE: incremented and printed, but the print is a normal per-event line, not shutdown.
    good2 = [
        "int g_refused = 0;",
        "",
        "void on_refuse() {",
        "    ++g_refused;",
        '    logf("refused (%d so far)", g_refused);',
        "}",
    ]
    hits4 = scan_file("synthetic_good2.cpp", lines=good2)
    check(
        "a counter printed from an ordinary per-event site is NOT flagged",
        not any(n == "g_refused" for n, _, _, _ in hits4),
    )

    # a counter incremented but never printed anywhere is a DIFFERENT lint's problem.
    good3 = ["int g_silent = 0;", "", "void bump() {", "    ++g_silent;", "}"]
    hits5 = scan_file("synthetic_good3.cpp", lines=good3)
    check("a counter with no print at all (any kind) is out of this lint's scope", not hits5)

    # the allow-list's own shape checks.
    check(
        "a clean allow-list entry passes",
        not allow_problems([{"path": "a.cpp", "counter": "g_x", "reason": "r", "tracker_id": "T"}]),
    )
    check("a malformed allow-list entry is refused", bool(allow_problems([{"path": "a.cpp"}])))
    check(
        "a duplicate allow-list entry is refused",
        bool(
            allow_problems(
                [
                    {"path": "a.cpp", "counter": "g_x", "reason": "r", "tracker_id": "T"},
                    {"path": "a.cpp", "counter": "g_x", "reason": "r2", "tracker_id": "T2"},
                ]
            )
        ),
    )

    a_probs = allow_problems(load_allow())
    check("the committed allow-list itself is well-formed", not a_probs, "; ".join(a_probs))

    offenders = run()
    check(
        "the committed tree is clean (no un-allow-listed offender)", not offenders, repr(offenders)
    )

    print("lint_shutdown_counters selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()

    a_probs = allow_problems(load_allow())
    if a_probs:
        print(
            "lint_shutdown_counters: %d problem(s) in %s:"
            % (len(a_probs), os.path.relpath(ALLOW_PATH, REPO))
        )
        for p in a_probs:
            print("  " + p)
        return 1

    offenders = run()
    if offenders:
        total = sum(len(v) for v in offenders.values())
        print("lint_shutdown_counters: %d counter(s) reported only at teardown:" % total)
        for rel, hits in sorted(offenders.items()):
            for name, incr_line, print_lines in hits:
                print(
                    "  %s: %s (incremented at line %d, printed only at line(s) %s)"
                    % (rel, name, incr_line, ", ".join(str(x) for x in print_lines))
                )
        print(
            "  fix: log the first occurrence + a periodic rollup (mh_common/include/mh_diag_counter.h), "
            "or accept it onto tools/data/shutdown_counter_allow.json with a reason"
        )
        return 1
    print("lint_shutdown_counters: PASS (0 shutdown-only counters)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
