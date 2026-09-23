#!/usr/bin/env python3
"""lint_ini_string_keys.py -- no STRING-valued key in mh_net.example.ini may carry a same-line comment.

THE TRAP (user ruling 2026-09-20, after the third bite). Win32 GetPrivateProfileStringA returns
everything after the `=` up to the end of the line -- a trailing `; comment` IS the value. The
example ini documents every key with exactly such a comment on the key's own line, which is fine for
a key read with GetPrivateProfileIntA (atoi stops at the first non-digit) and wrong for every key
read as a STRING:

  * `; relay=HOST:PORT   ; UDP only: ...` -- the first rc3 player uncommented it, filled in the VPS
    and got `does not resolve` (2026-09-19; the reader now trims, log_formats net.relay_trailing_comment).
  * `probe_text=   ; TEST AFFORDANCE, empty = off ...` -- a STOCK ini drew the comment text on every
    present, because the empty value was not empty (2026-09-20).
  * `[config] mode` / `[net] module` / `[net] transport` -- strict compares that REFUSE the run
    (the F5F doc proof), which is why those three lines already carry a NO-TRAILING-COMMENT warning.

Prose warnings did not hold (the probe_text line was written after the [patch] manifest= line that
documents the very trap), so this is a gate. The rule is mechanical and derived, not a list:

  STRING-VALUED  = every (section, key) some GetPrivateProfileStringA("section", "key", ...) call in
                   src/mh_dll reads. Re-derived on every run by scanning the sources, so a new string
                   key is covered the moment its reader is written.
  VIOLATION      = a line in the example ini that sets such a key -- LIVE (`key=value`) or
                   COMMENTED-OUT (`;key=value`, `; key=value`: the form a user is invited to
                   uncomment, which is exactly how the relay line bit) -- with a `;` anywhere after
                   the `=`.

Integer keys (GetPrivateProfileIntA) are out of scope and keep their same-line comments: atoi never
reads past the number. The fix for a violation is always the same: move the comment to its own line
ABOVE the key, and leave the value line bare.

    python tools/lint_ini_string_keys.py             # the gate (a lint_repo row)
    python tools/lint_ini_string_keys.py --list      # print the derived string-key set
    python tools/lint_ini_string_keys.py --selftest  # planted violations go RED, clean text green
"""

import argparse
import io
import os
import re
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_INI = os.path.join(REPO, "src", "mh_dll", "mh_net.example.ini")
DEFAULT_SRC = os.path.join(REPO, "src", "mh_dll")

# The reader call, with BOTH arguments literal. A reader that builds its key name at runtime
# (gfx_overlay's `page.<name>.items`) is not a fixed key and cannot be listed in the example ini
# anyway; the `pages=` list that names those pages is a literal read and IS covered.
#
# TWO SPELLINGS, because TL-HARN4 (2026-09-21) routed most STRING reads through the shared
# `mh::config::read_ini_string` helper (config/ini_read.h), a same-signature drop-in for
# GetPrivateProfileStringA -- and this regex derives its "string key" set by literally matching
# the call token, so a site rewritten to the wrapper would silently drop out of the derived set
# (the gate would stop protecting exactly the keys the durable fix just made safe) unless the
# wrapper is matched too. `read_ini_string` alone also matches the one call site that invokes it
# unqualified from inside the `mh::config` namespace itself (config/config.h's `[config] mode`).
READER_RE = re.compile(
    r'(?:GetPrivateProfileStringA|(?:mh::config::)?read_ini_string)'
    r'\s*\(\s*"([A-Za-z_0-9]+)"\s*,\s*"([A-Za-z_0-9.]+)"'
)
SOURCE_EXT = (".cpp", ".c", ".h")
# Build output and vendored trees under src/mh_dll; nothing in them reads an ini.
SKIP_DIRS = {"Release", "Release_asan", "Debug", "Win32", "x64", ".vs", "obj"}

SECTION_RE = re.compile(r"^\s*\[([^\]]+)\]\s*$")
# A key line, live or commented out. The commented-out form is deliberately NARROW -- `;key=` or
# `; key=` -- because the ini's prose also contains `k=v` fragments inside sentences, and those are
# not lines a user uncomments.
KEY_RE = re.compile(r"^(?P<lead>;\s?)?(?P<key>[A-Za-z_][A-Za-z_0-9.]*)\s*=(?P<rest>.*)$")


def string_keys(src_dir):
    keys = set()
    for root, dirs, files in os.walk(src_dir):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for fn in files:
            if not fn.endswith(SOURCE_EXT):
                continue
            path = os.path.join(root, fn)
            with io.open(path, encoding="utf-8", errors="replace") as fh:
                for m in READER_RE.finditer(fh.read()):
                    keys.add((m.group(1).lower(), m.group(2).lower()))
    return keys


def find_violations(ini_text, keys):
    """[(lineno, section, key, line)] for every string-key line with a `;` after its `=`."""
    out = []
    section = None
    for i, raw in enumerate(ini_text.split("\n"), 1):
        line = raw.rstrip("\r")
        m = SECTION_RE.match(line)
        if m:
            section = m.group(1).strip().lower()
            continue
        if section is None:
            continue
        m = KEY_RE.match(line)
        if not m:
            continue
        key = m.group("key").lower()
        if (section, key) not in keys:
            continue
        if ";" in m.group("rest"):
            out.append((i, section, key, line))
    return out


def _rel(path):
    """Repo-relative for the message; the absolute path when it is not under the repo (the selftest
    plants its fixture in a temp dir, which on Windows can be on another drive)."""
    try:
        return os.path.relpath(path, REPO).replace("\\", "/")
    except ValueError:
        return path.replace("\\", "/")


def run(ini_path, src_dir, say=print):
    keys = string_keys(src_dir)
    if not keys:
        say(
            "lint_ini_string_keys: FAIL -- no GetPrivateProfileStringA reader found under %s"
            % src_dir
        )
        return 1
    with io.open(ini_path, encoding="utf-8") as fh:
        text = fh.read()
    bad = find_violations(text, keys)
    for ln, sec, key, line in bad:
        say(
            "%s:%d: [%s] %s is read as a STRING (GetPrivateProfileStringA) and carries a same-line "
            "comment -- the comment IS the value. Move it to its own line above the key.\n    %s"
            % (_rel(ini_path), ln, sec, key, line.strip())
        )
    if bad:
        say(
            "lint_ini_string_keys: FAIL -- %d string-key line(s) with a same-line comment"
            % len(bad)
        )
        return 1
    say(
        "lint_ini_string_keys: ok -- %d string keys derived from the readers, none with a same-line "
        "comment in %s" % (len(keys), _rel(ini_path))
    )
    return 0


def selftest():
    fails = []

    def expect(name, cond):
        print("  [%s] %s" % ("ok" if cond else "FAIL", name))
        if not cond:
            fails.append(name)

    # The real tree: the derived set is non-trivial and contains the keys this lint was written for.
    keys = string_keys(DEFAULT_SRC)
    expect("the reader scan finds a string-key set (%d)" % len(keys), len(keys) >= 20)
    for want in (
        ("fonts", "probe_text"),
        ("net", "relay"),
        ("net", "transport"),
        ("config", "mode"),
    ):
        expect("derived set contains [%s] %s" % want, want in keys)
    expect("an int key is NOT in the set ([net] port)", ("net", "port") not in keys)

    tmp = tempfile.mkdtemp(prefix="mh_inilint_")
    try:
        # A fake source tree with one string reader and one int reader.
        src = os.path.join(tmp, "src")
        os.makedirs(src)
        with io.open(os.path.join(src, "reader.cpp"), "w", encoding="utf-8") as fh:
            fh.write(
                'GetPrivateProfileStringA("zz", "name", "", buf, sizeof(buf), ini);\n'
                'GetPrivateProfileIntA("zz", "count", 0, ini);\n'
            )
        k = string_keys(src)
        expect("fake tree: exactly the string key is derived", k == {("zz", "name")})

        def viol(text):
            return find_violations(text, k)

        expect("a bare string key is clean", viol("[zz]\nname=abc\n") == [])
        expect(
            "comment on its own line above the key is clean",
            viol("[zz]\n; what it is\nname=abc\n") == [],
        )
        expect(
            "an int key with a same-line comment is clean", viol("[zz]\ncount=3 ; three\n") == []
        )
        expect(
            "a LIVE string key with a same-line comment is RED",
            [v[2] for v in viol("[zz]\nname=abc   ; a comment\n")] == ["name"],
        )
        expect(
            "an EMPTY string value with a same-line comment is RED (the probe_text shape)",
            [v[2] for v in viol("[zz]\nname=   ; empty = off\n")] == ["name"],
        )
        expect(
            "a COMMENTED-OUT string key with a same-line comment is RED (the relay shape)",
            [v[2] for v in viol("[zz]\n;name=HOST:PORT   ; the relay\n")] == ["name"]
            and [v[2] for v in viol("[zz]\n; name=HOST:PORT   ; the relay\n")] == ["name"],
        )
        expect(
            "the same key in ANOTHER section is not a string key there",
            viol("[other]\nname=abc ; fine\n") == [],
        )
        expect(
            "prose that merely mentions `name=` mid-sentence is not a key line",
            viol("[zz]\n;   e.g. name=abc ; in prose\n") == [],
        )
        # The gate's exit code, over a planted file.
        ini = os.path.join(tmp, "x.ini")
        with io.open(ini, "w", encoding="utf-8") as fh:
            fh.write("[zz]\nname=abc ; planted\n")
        expect("run() exits 1 on a planted violation", run(ini, src, say=lambda *a: None) == 1)
        with io.open(ini, "w", encoding="utf-8") as fh:
            fh.write("[zz]\n; planted, on its own line\nname=abc\n")
        expect(
            "run() exits 0 once the comment is moved above the key",
            run(ini, src, say=lambda *a: None) == 0,
        )
    finally:
        import shutil

        shutil.rmtree(tmp, ignore_errors=True)

    # And the committed example ini itself is clean -- the selftest is not a substitute for the gate,
    # but a selftest that passes while the gate is red would be the vacuous kind.
    expect(
        "the committed example ini passes the gate",
        run(DEFAULT_INI, DEFAULT_SRC, say=lambda *a: None) == 0,
    )

    print("lint_ini_string_keys selftest: %d failure(s)" % len(fails))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ini", default=DEFAULT_INI)
    ap.add_argument("--src", default=DEFAULT_SRC)
    ap.add_argument("--list", action="store_true", help="print the derived string-key set")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.list:
        for sec, key in sorted(string_keys(args.src)):
            print("[%s] %s" % (sec, key))
        return 0
    return run(args.ini, args.src)


if __name__ == "__main__":
    sys.exit(main())
