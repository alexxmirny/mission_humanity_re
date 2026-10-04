#!/usr/bin/env python3
"""lint_log_sink -- no per-line log writer outside the async log sink (mp:LOG1, done_when (a)).

WHY. User ruling 2026-10-04: no log I/O on the game's main/sim/present thread. Until mp:LOG1 about
thirty sites each did CreateFile(FILE_APPEND_DATA) -> WriteFile -> CloseHandle per line; with real-time
antivirus on, each close of a modified multi-MB file can be scanned, and a field match showed
0.7-1.6 s main-thread hitches between two consecutive log lines. Every log line in the game process
now goes through ONE sink (src/mh_dll/mh_common/include/mh_log_sink.h + src/mh_dll/mh_common/mh_log_sink.cpp) that enqueues and lets a writer
thread own the handles. A prose rule would not hold -- the next author copies the nearest log helper,
which is exactly how thirty copies of this pattern came to exist -- so this is the gate.

WHAT IT FLAGS (in src/mh_dll, comments stripped, so prose cannot trip it):
  * FILE_APPEND_DATA -- the append-open every old writer used;
  * CreateFile[A/W](... OPEN_ALWAYS ...) -- the other spelling of "open for append";
  * fopen/fopen_s/_wfopen with an append mode, and std::ios::app / ios_base::app.
Allowed in exactly the files in ALLOW below (the sink itself, and the rotation helper whose
reopen-after-failed-rename arm is part of the same policy). The selftests (mh_nettest, libmh_test)
are out of scope: they read and write their own scratch files and are not the game process.

NOT FLAGGED, ON PURPOSE: one-shot CREATE_ALWAYS/GENERIC_WRITE writers (the pre-terminate
`mh_config_refused.log`, binary recordings such as orders/clock streams, snapshots). Those are not
per-line append loggers; widening the net to every CreateFile would drown the gate in false positives
and train people to wave it through.

SECOND RULE (mp:LOG2, 2026-10-04): the BINARY recordings. The harness's per-step order/clock streams,
the match seed, its snapshots and the received map used to be CreateFile(CREATE_ALWAYS) + WriteFile on
the sim/game thread -- ~100 writes/s in a net-debug build. They now go through the same sink
(mh_logq_create_bin / mh_logq_write_bin), and in the RECORD_TUS files below a direct WriteFile,
CREATE_ALWAYS, SetFilePointer or fwrite fails unless the line (or one of the 3 above it) carries a
`LOG-SINK-OK: <reason>` marker. The marker is how a console-handle write (a refusal shouted to stderr)
is told from a recording, and it forces the author to say why in the diff.

  python tools/lint_log_sink.py            # the gate (a lint_repo row)
  python tools/lint_log_sink.py --selftest # plant each failure and require it to go red
"""

import argparse
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cstrip  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src", "mh_dll")

# Directories that are NOT the game process: the offline oracles and the host tools.
SKIP_DIRS = {
    "mh_nettest",
    "libmh_test",
    "libref_host",
    "mh_tools",
    "Release",
    "Debug",
    "x64",
    "x86",
}

# repo-relative (forward slashes) -> why it may open a log for append.
ALLOW = {
    "src/mh_dll/mh_common/mh_log_sink.cpp": "the sink's writer: the ONE place that appends to a log",
    "src/mh_dll/mh_common/include/mh_log_sink.h": "the sink's header-only client: its synchronous fallback when no sink is published",
    "src/mh_dll/mh_common/include/mh_log_rotate.h": "the rotation policy helper (reopen after a lost rename); callers are the sink",
    "src/mh_dll/include/miniz/miniz.c": "vendored miniz 3.0.2 (mp:D46): its zip-archive stdio helper, not a log writer; kept byte-identical to upstream",
}

# mp:LOG2: the translation units that hold the game-thread recordings (see SECOND RULE above).
RECORD_TUS = {
    "src/mh_dll/mh_harness/harness.cpp": "orders/clock/seed/snapshot recordings, match segment",
    "src/mh_dll/mh/seams/map_transfer.cpp": "the received-map save",
    "src/mh_dll/mh/seams/net_discovery.cpp": "session.json",
}
RECORD_RULES = [
    ("WriteFile on a game thread", re.compile(r"\bWriteFile\s*\(")),
    ("CreateFile(... CREATE_ALWAYS ...)", re.compile(r"\bCREATE_ALWAYS\b")),
    ("SetFilePointer (a seek-back patch)", re.compile(r"\bSetFilePointer(?:Ex)?\s*\(")),
    ("fwrite", re.compile(r"\bfwrite\s*\(")),
]
OK_MARK = "LOG-SINK-OK"

EXTS = (".cpp", ".c", ".h", ".hpp", ".cc", ".inl")

RULES = [
    ("FILE_APPEND_DATA", re.compile(r"\bFILE_APPEND_DATA\b")),
    (
        "CreateFile(... OPEN_ALWAYS ...)",
        re.compile(r"\bCreateFile[AW]?\s*\([^;]*?\bOPEN_ALWAYS\b", re.S),
    ),
    (
        "fopen in append mode",
        re.compile(r"\b_?w?fopen(?:_s)?\s*\([^;]*?\"[^\"]*a[^\"]*\"\s*\)", re.S),
    ),
    ("ios::app", re.compile(r"\bios(?:_base)?::app\b")),
]


def scan_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    stripped = _cstrip.strip_comments(text, keep_lines=True)
    hits = []
    for name, rx in RULES:
        for m in rx.finditer(stripped):
            line = stripped.count("\n", 0, m.start()) + 1
            hits.append((name, line))
    return hits


def scan_record_file(path):
    """The SECOND RULE for one RECORD_TUS file: direct binary writers without a LOG-SINK-OK marker."""
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    raw_lines = text.split("\n")
    stripped = _cstrip.strip_comments(text, keep_lines=True)
    hits = []
    for name, rx in RECORD_RULES:
        for m in rx.finditer(stripped):
            line = stripped.count("\n", 0, m.start()) + 1
            near = raw_lines[max(0, line - 4) : line]  # this line and the 3 above it
            if any(OK_MARK in ln for ln in near):
                continue
            hits.append((name, line))
    return hits


def scan(root=SRC, repo=REPO, allow=ALLOW, record_tus=None):
    bad = []
    rec = RECORD_TUS if record_tus is None else record_tus
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if not fn.endswith(EXTS):
                continue
            p = os.path.join(dirpath, fn)
            rel = os.path.relpath(p, repo).replace("\\", "/")
            if rel in rec:
                for name, line in scan_record_file(p):
                    bad.append((rel, line, name))
            if rel in allow:
                continue
            for name, line in scan_file(p):
                bad.append((rel, line, name))
    return sorted(bad)


def run():
    bad = scan()
    if bad:
        print(
            "lint_log_sink: FAIL -- %d log/recording writer(s) outside the async sink:" % len(bad)
        )
        for rel, line, name in bad:
            print("  %s:%d  %s" % (rel, line, name))
        print(
            "  Log through mh_logq_write()/mh_logq_write2() (src/mh_dll/mh_common/include/mh_log_sink.h): "
            "enqueue the formatted line + its path; never open a log on a game thread. A BINARY recording "
            "goes through mh_logq_create_bin()/mh_logq_write_bin(); a deliberate console/one-shot write "
            "in a RECORD_TUS file needs a `LOG-SINK-OK: <reason>` marker on or just above it."
        )
        return 1
    print(
        "lint_log_sink: OK (no per-line log writer outside the sink; %d allowlisted file(s))"
        % len(ALLOW)
        + "; %d recording TU(s) free of direct binary writers" % len(RECORD_TUS)
    )
    return 0


def selftest():
    fails = 0

    def expect(label, files, want_bad):
        nonlocal fails
        with tempfile.TemporaryDirectory() as td:
            root = os.path.join(td, "src", "mh_dll")
            for rel, body in files.items():
                p = os.path.join(td, rel)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "w", encoding="utf-8") as fh:
                    fh.write(body)
            got = scan(
                root=root,
                repo=td,
                allow={"src/mh_dll/mh_common/mh_log_sink.cpp": "x"},
                record_tus={"src/mh_dll/mh_harness/harness.cpp": "x"},  # CITATION-OK: planted path
            )
            ok = bool(got) == want_bad
            print("  [%s] %s" % ("ok" if ok else "FAIL", label))
            if not ok:
                fails += 1

    bad = "src/mh_dll/mh/seams/x.cpp"  # CITATION-OK: planted path
    expect(
        "a planted FILE_APPEND_DATA writer goes RED",
        {bad: 'void f(){ CreateFileA(p, FILE_APPEND_DATA, 0, 0, 0, 0, 0); }'},
        True,
    )
    expect(
        "a planted OPEN_ALWAYS writer (no FILE_APPEND_DATA) goes RED",
        {bad: "void f(){ CreateFileA(p, GENERIC_WRITE,\n 0, nullptr, OPEN_ALWAYS, 0, nullptr); }"},
        True,
    )
    expect(
        "a planted fopen(\"a\") goes RED", {bad: 'void f(){ FILE* x = fopen("a.log", "a"); }'}, True
    )
    expect(
        "a planted std::ios::app goes RED",
        {bad: "void f(){ std::ofstream o(p, std::ios::app); }"},
        True,
    )
    expect(
        "the same text inside a comment is NOT a violation",
        {bad: "// CreateFileA(p, FILE_APPEND_DATA, ...)\n/* OPEN_ALWAYS */ void f(){}"},
        False,
    )
    expect(
        "a one-shot CREATE_ALWAYS writer is NOT a violation",
        {bad: "void f(){ CreateFileA(p, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0); }"},
        False,
    )
    expect(
        "the allowlisted sink file is exempt",
        {
            "src/mh_dll/mh_common/mh_log_sink.cpp": "void f(){ CreateFileA(p, FILE_APPEND_DATA, 0,0,OPEN_ALWAYS,0,0); }"
        },
        False,
    )
    expect(
        "a selftest directory is out of scope",
        {
            "src/mh_dll/mh_nettest/t.cpp": "void f(){ CreateFileA(p, FILE_APPEND_DATA, 0,0,OPEN_ALWAYS,0,0); }"  # CITATION-OK: planted path
        },
        False,
    )
    rec = "src/mh_dll/mh_harness/harness.cpp"  # CITATION-OK: planted path
    expect(
        "a planted per-record WriteFile in a recording TU goes RED",
        {rec: "void order_record(){ WriteFile(g_rec_h, &r, sizeof(r), &w, nullptr); }"},
        True,
    )
    expect(
        "a planted CREATE_ALWAYS recording in a recording TU goes RED",
        {rec: "void f(){ CreateFileA(p, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0); }"},
        True,
    )
    expect(
        "a planted seek-back patch in a recording TU goes RED",
        {rec: "void f(){ SetFilePointer(h, 0, nullptr, FILE_BEGIN); }"},
        True,
    )
    expect(
        "a WriteFile marked LOG-SINK-OK (console handle) is NOT a violation",
        {
            rec: "void f(){\n  // LOG-SINK-OK: stderr console handle, not a file\n  WriteFile(e, line, n, &w, nullptr);\n}"
        },
        False,
    )
    expect(
        "a marker too far above does NOT excuse a later WriteFile",
        {
            rec: "// LOG-SINK-OK: x\nint a;\nint b;\nint c;\nint d;\nvoid f(){ WriteFile(e, l, n, &w, nullptr); }"
        },
        True,
    )
    expect(
        "WriteFile in a file OUTSIDE the recording set is NOT a violation of the second rule",
        {bad: "void f(){ WriteFile(e, l, n, &w, nullptr); }"},
        False,
    )
    print("=== lint_log_sink --selftest: %d failure(s) ===" % fails)
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument(
        "--selftest", action="store_true", help="plant each failure and require it to go red"
    )
    args = ap.parse_args()
    return selftest() if args.selftest else run()


if __name__ == "__main__":
    sys.exit(main())
