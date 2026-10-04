#!/usr/bin/env python3
"""mods:LANG4 -- every player-visible string mh.dll draws goes through the string table.

The table is src/mh_dll/mh/ui/player_strings.def (English compiled in; a language pack's
lang/<id>/mh_strings.txt translates it per key). A literal drawn for a PLAYER that bypasses it is a
string no pack can translate -- the RU player sees English in the middle of Russian. Debug overlay,
ImGui and log text stay English by ruling (user, 2026-09-29) and are out of scope.

Three checks:

1. **Wide literals, all of mh.dll.** mh.dll draws player text through the game's UTF-16 widget/font
   path, so a wide literal (L"...") is the shape a player-visible string takes. Any wide literal with
   two consecutive ASCII letters outside player_strings.def is a finding, unless its line carries a
   `mh-str-ok: <why>` marker (a font face name, a DLL name, a symbol that is not language). Comments
   are stripped first (tools/_cstrip.py), so an L"..." quoted in prose is not a finding.
2. **Narrow literals in the DISPLAY modules** (DISPLAY_MODULES below: the files that draw the lobby
   status line, the refusals, the map notices, the desync alert, the cheat refusal, the connection
   indicator and the fallback menu label). There a narrow literal with two letters is a finding
   unless it is a log line (starts with ';', or its statement calls a log function), or its line is
   marked `mh-str-ok:`.
3. **Every pack strings file** (src/formats/mh_strings/*.txt) against the .def: unknown or duplicate
   keys, empty texts, and a printf shape that differs from the English (src/formats/langpack.py
   strings_problems, the same rule mh.dll's loader applies).

    python tools/lint_player_strings.py             # scan
    python tools/lint_player_strings.py --selftest  # planted literals must go RED, marked ones stay green
"""

from __future__ import annotations

import argparse
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, os.path.join(REPO, "src", "formats"))

import _cstrip  # noqa: E402

MH = os.path.join(REPO, "src", "mh_dll", "mh")
DEF = os.path.join(MH, "ui", "player_strings.def")
PACK_STRINGS = os.path.join(REPO, "src", "formats", "mh_strings")
MARK = "mh-str-ok:"

# Not player text by construction: the reimpl probe's UTF-16 round-trip corpus (test vectors).
EXCLUDE = {
    os.path.join("seams", "reimpl_probe.cpp"),
}
EXCLUDE_DIRS = ("overlay", "imgui")  # the debug overlay and ImGui: English by ruling

DISPLAY_MODULES = (
    os.path.join("seams", "ui_net_indicator.cpp"),
    os.path.join("ui", "lobby_notice.cpp"),
    os.path.join("seams", "map_transfer.cpp"),
    os.path.join("desync", "desync_watch.cpp"),
    os.path.join("seams", "mp_menu.cpp"),
    os.path.join("seams", "ui_cheat_gate.cpp"),
)
# A statement that calls one of these is a log/diagnostic line, not player text.
LOG_CALL = re.compile(r"\b(\w*(log|Log|LOG|trace|dbg|diag|OutputDebugString)\w*|f?printf)\s*\(")

# ...and a narrow literal only reaches a player through a text builder: composed into a buffer that
# is widened and drawn (the lobby line, the indicator's name). Config keys, pragmas and file names
# never pass through one of these.
TEXT_SINK = re.compile(
    r"\b(wsprintfA|wvsprintfA|lstrcpyA|lstrcpynA|lstrcatA|\w*sprintf\w*|\w*ansi_to_wide\w*|MultiByteToWideChar)\s*\("
)
PATHLIKE = re.compile(r"\.(log|ini|txt|gfx|dat|rsr|nam|dll|exe|lib|bin|json|csv)\b|\\\\")

LOG_PREFIX = re.compile(r'(?<![A-Za-z0-9_])"; ')

WIDE = re.compile(r'(?<![A-Za-z0-9_])L"((?:[^"\\\n]|\\.)*)"')
NARROW = re.compile(r'(?<![A-Za-z0-9_])"((?:[^"\\\n]|\\.)*)"')
LETTERS = re.compile(r"[A-Za-z]{2}")


def _statement(lines, i):
    """The whole statement line i belongs to: back to the previous line ending in ';' '{' '}', forward
    to the line that ends it -- a wrapped argument list is judged with the call it belongs to."""
    out = [lines[i]]
    j = i - 1
    while j >= 0 and not re.search(r"[;{}]\s*$", lines[j]) and len(out) < 30:
        out.insert(0, lines[j])
        j -= 1
    k = i
    while k + 1 < len(lines) and not re.search(r"[;{}]\s*$", lines[k]) and len(out) < 60:
        k += 1
        out.append(lines[k])
    return " ".join(out)


def scan_text(rel, text, display):
    """Findings for one source file's text: [(rel, line, message)]."""
    raw = text.splitlines()
    code = _cstrip.strip_comments(text, keep_lines=True).splitlines()
    found = []
    for i, line in enumerate(code):
        marked = i < len(raw) and MARK in raw[i]
        for m in WIDE.finditer(line):
            if LETTERS.search(m.group(1)) and not marked:
                found.append(
                    (
                        rel,
                        i + 1,
                        f'wide literal L"{m.group(1)[:40]}" bypasses ui/player_strings.def',
                    )
                )
        if not display:
            continue
        stripped = WIDE.sub("", line)
        for m in NARROW.finditer(stripped):
            s = m.group(1)
            if not LETTERS.search(s) or s.startswith(";") or marked:
                continue
            st = _statement(code, i)
            if not TEXT_SINK.search(st) or LOG_CALL.search(st) or PATHLIKE.search(s):
                continue
            if LOG_PREFIX.search(st):  # the repo's log-line convention: a "; [tag] ..." format
                continue
            found.append(
                (rel, i + 1, f'narrow literal "{s[:40]}" in a display module bypasses the table')
            )
    return found


def iter_sources():
    for root, dirs, files in os.walk(MH):
        dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
        for f in files:
            if not f.endswith((".cpp", ".h", ".inl", ".hpp")):
                continue
            p = os.path.join(root, f)
            rel = os.path.relpath(p, MH)
            if rel in EXCLUDE or f == "player_strings.def":
                continue
            yield rel, p


def scan():
    found = []
    for rel, p in iter_sources():
        with open(p, encoding="utf-8", errors="replace") as fh:
            found += scan_text(rel, fh.read(), rel in DISPLAY_MODULES)
    import langpack

    rows = langpack.read_string_def(DEF)
    if not rows:
        found.append(("ui/player_strings.def", 0, "no MH_STR rows parsed"))
    for f in sorted(os.listdir(PACK_STRINGS)):
        if f.endswith(".txt"):
            for prob in langpack.strings_problems(os.path.join(PACK_STRINGS, f), rows):
                found.append((os.path.join("src", "formats", "mh_strings", f), 0, prob))
    return found, len(rows)


def selftest():
    fails = []
    cases = [
        # (text, display module?, expected finding count, what)
        ('void f() { draw(L"Game over"); }\n', False, 1, "a planted wide literal"),
        (
            'void f() { draw(L"Game over"); } // mh-str-ok: test\n',
            False,
            0,
            "a marked wide literal",
        ),
        ('// draw(L"Game over") in prose\nvoid f() {}\n', False, 0, "a wide literal in a comment"),
        (
            'void f() { draw(L"P%d "); draw(L"R "); }\n',
            False,
            0,
            "symbols with fewer than two letters",
        ),
        (
            'void f() { wsprintfA(b, "Host left"); }\n',
            True,
            1,
            "a planted narrow literal in a display module",
        ),
        (
            'void f() { wsprintfA(b, "Host left"); }\n',
            False,
            0,
            "the same narrow literal outside one",
        ),
        (
            'void f() { read_ini("hud", "net_indicator"); }\n',
            True,
            0,
            "a config key (no text builder)",
        ),
        ('void f() { wsprintfA(p, "%smh_net.log", d); }\n', True, 0, "a file name"),
        ('void f() { log_line("; [net] host left %d", 1); }\n', True, 0, "a log line"),
        (
            'void f() { wsprintfA(m, "host left %d", 1); net_log(m); }\n',
            True,
            0,
            "a log line's builder",
        ),
        (
            'void f() {\n    net_log(g,\n            "host left");\n}\n',
            True,
            0,
            "a wrapped log call",
        ),
        (
            'void f() {\n    wsprintfA(m, "; [map] %s",\n              ok ? "held" : "refused");\n}\n',
            True,
            0,
            "an argument of a '; ' log format",
        ),
    ]
    for text, display, want, what in cases:
        got = scan_text("planted.cpp", text, display)
        ok = len(got) == want
        print(f"  {'OK ' if ok else 'FAIL'} {what}: {len(got)} finding(s), want {want}")
        if not ok:
            fails.append(what)
    import langpack
    import tempfile

    rows = langpack.read_string_def(DEF)
    d = tempfile.mkdtemp(prefix="mh_lps_")
    bad = os.path.join(d, "xx.txt")
    with open(bad, "w", encoding="utf-8") as fh:
        fh.write("lobby.refused = %d\n")
    probs = langpack.strings_problems(bad, rows)
    os.remove(bad)
    os.rmdir(d)
    ok = any("takes" in p for p in probs)
    print(f"  {'OK ' if ok else 'FAIL'} a pack file with a wrong printf shape is a finding")
    if not ok:
        fails.append("pack shape")
    found, n = scan()
    print(f"  {'OK ' if not found else 'FAIL'} the committed tree is clean ({n} table rows)")
    if found:
        fails.append("tree not clean")
    print("lint_player_strings selftest " + ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 1 if fails else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    found, n = scan()
    for rel, line, msg in found:
        print(f"{rel.replace(os.sep, '/')}:{line}: {msg}")
    if found:
        print(
            f"lint_player_strings: {len(found)} finding(s) -- route player text through ui/player_strings.def "
            f"(tr(Str::...)), or mark a non-language literal's line `{MARK} <why>`"
        )
        return 1
    print(f"lint_player_strings: OK ({n} table rows; no player-visible literal bypasses the table)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
