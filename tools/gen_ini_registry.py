#!/usr/bin/env python3
"""gen_ini_registry.py -- the mh_net.ini key REGISTRY: lint, launcher schema, example-ini check.

The registry is src/mh_dll/mh/config/ini_keys.def: an X-macro file with one MH_INI_KEY(...) row per
key (section, key, type, default, class, label, sub-tab, options, flags). Its column contract is
written at the top of that file. This tool is the Python reader of the same rows; C++ includes the
file with its own macro (mh_common/include/mh_ini_gate.h, the RL2 ship gate).

    python tools/gen_ini_registry.py --check      # the lint_repo row: registry <-> code <-> example ini
    python tools/gen_ini_registry.py --write      # regenerate src/launcher/settings_schema.json
    python tools/gen_ini_registry.py --list       # print the registry grouped by class
    python tools/gen_ini_registry.py --selftest   # a planted unregistered key goes RED

--check fails on:
  * a key READ in src/mh_dll (GetPrivateProfileIntA / GetPrivateProfileStringA / mh_ini_get_int / mh_ini_get_str /
    read_ini_string / ini_int / net_ini_int / net_ini_str / mh_log_cap_bytes / the gfx_overlay load_key wrapper) with no
    registry row;
  * a registry row that nothing reads (except a row flagged F_PENDING: registered ahead of its reader);
  * a read whose section or key is not a string literal and is not one of the known wrapper bodies
    (DYNAMIC_OK) -- an unreadable call site is a hole in the check, so it is reported, not skipped;
  * a key named in mh_net.example.ini (live `k=v` or commented-out `;k=v`) that is not registered, or
    a `user`/`fixed` registry key that the example ini does not document;
  * a malformed row (unknown enumerator, duplicate key, enum without options, user row without a
    label / sub-tab, non-user row with either, bool/int default that is not a number);
  * a committed src/launcher/settings_schema.json that differs from what --write would produce.

Test code (mh_nettest, libmh_test) is not scanned: its fixtures read made-up keys on purpose.
"""

import argparse
import json
import os
import re
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DEF = os.path.join(REPO, "src", "mh_dll", "mh", "config", "ini_keys.def")
DEFAULT_SRC = os.path.join(REPO, "src", "mh_dll")
DEFAULT_INI = os.path.join(REPO, "src", "mh_dll", "mh_net.example.ini")
DEFAULT_SCHEMA = os.path.join(REPO, "src", "launcher", "settings_schema.json")

SOURCE_EXT = (".cpp", ".c", ".h", ".inl")
SKIP_DIRS = {
    "Release",
    "Release_asan",
    "Debug",
    "Win32",
    "x64",
    ".vs",
    "obj",
    "mh_nettest",
    "libmh_test",
}

TYPES = {
    "T_INT": "int",
    "T_BOOL": "bool",
    "T_STRING": "string",
    "T_ENUM": "enum",
    "T_HOTKEY": "hotkey",
    "T_FLOAT": "float",
}
CLASSES = {"C_USER": "user", "C_DEV": "dev", "C_FIXED": "fixed"}
SUBTABS = {
    "S_NONE": None,
    "S_DISPLAY": "display",
    "S_LANGUAGE": "language",
    "S_MULTIPLAYER": "multiplayer",
    "S_GAMEPLAY": "gameplay",
    "S_DIAGNOSTICS": "diagnostics",
    "S_UPDATES": "updates",
}
FLAGS = {
    "F_NONE": None,
    "F_EXPERIMENTAL": "experimental",
    "F_RESTART": "restart",
    "F_PENDING": "pending",
    "F_HOSTSYNC": "host",
}
SUBTAB_ORDER = ["display", "language", "multiplayer", "gameplay", "diagnostics", "updates"]

# PT-SET1: F_HOSTSYNC rows are read ONLY through the X-macro in this header, and each one needs exactly
# one apply hook entry {"sec","key",[](...)...} in HOOKS_FILE.
GAMEPLAY_HEADER = "mh_common/include/mh_gameplay.h"
HOOKS_FILE = "mh/seams/gameplay_hooks.cpp"
HOOK_RE = re.compile(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*\[')

# Reader call tokens -> how to find (section, key) in the argument list.
#   sec/key = argument index, or a fixed string (the wrapper hard-wires the section).
READERS = {
    "GetPrivateProfileIntA": (0, 1),
    "GetPrivateProfileStringA": (0, 1),
    "mh_ini_get_int": (0, 1),  # RL2: the ship-gated wrappers (mh_common/include/mh_ini_gate.h)
    "mh_ini_get_str": (0, 1),
    "read_ini_string": (0, 1),
    "ini_int": (0, 1),
    "net_ini_int": ("net", 0),
    "net_ini_str": ("net", 0),
    "mh_log_cap_bytes": (1, 2),  # (ini, section, key, default_mb)
    "load_key": ("debug", 1),  # gfx_overlay.cpp: load_key(ini, key, def, ...) reads [debug]
}
READER_RE = re.compile(r"\b(" + "|".join(READERS) + r")\s*\(")

# Call sites whose section/key is a variable BY DESIGN: the bodies of the wrappers above, and two
# tables. (file suffix, section arg text, key arg text). Anything else non-literal is reported.
DYNAMIC_OK = {
    ("mh/seams/launch.cpp", "section", "key"),  # ini_int body
    ("mh/seams/launch.cpp", '"net"', "key"),  # net_ini_str body
    ("mh/config/ini_read.h", "section", "key"),  # read_ini_string body
    ("mh_common/include/mh_ini_gate.h", "section", "key"),  # the gate's own reads (RL2)
    ("mh_common/include/mh_gameplay.h", "r.sec", "r.key"),  # PT-SET1: X-macro over F_HOSTSYNC rows
    ("mh_common/include/mh_log_rotate.h", "section", "key"),  # mh_log_cap_bytes body
    ("mh/seams/gfx_overlay.cpp", '"debug"', "key"),  # load_key body
    ("mh_harness/harness.cpp", '"harness"', "k.key"),  # refusal loop over the spine-key table
    ("mh/seams/net_lockstep.cpp", '"net"', "gone.name"),  # RETIRED_KNOBS probe, not a setting
}


# Literal reads of a file that is NOT mh_net.ini -- they are not settings of ours and need no row.
# (file suffix, section, key).
OTHER_FILE_OK = {
    ("mh/seams/lang_pack.cpp", "pack", "codepage"),  # lang\<id>\pack.ini, written by langpack.py
}


# Example-ini keys whose NAME is built at run time (gfx_overlay: page.<name>.title / .items). They are
# not settings and cannot be rows; they are the only non-registry keys the example ini may carry.
INI_DYNAMIC = [("debug", re.compile(r"page\.[A-Za-z_0-9]+\.(title|items)$"))]


class Row:
    __slots__ = (
        "section",
        "key",
        "type",
        "default",
        "cls",
        "label",
        "subtab",
        "options",
        "flags",
        "line",
        "lo",
        "hi",
    )

    def ident(self):
        return (self.section, self.key)


# ------------------------------------------------------------------------------------ registry ---


def _strip_comments(text):
    """Blank out // and /* */ comments (keeping newlines, so line numbers hold) and leave string and
    char literals intact."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i : j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def split_args(text, start):
    """text[start] is just past an opening '('. Return (args, end_index_after_close_paren) with args
    split on top-level commas, string literals respected."""
    args, cur, depth, i, n = [], [], 1, start, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            cur.append(text[i : j + 1])
            i = j + 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append("".join(cur).strip())
                return args, i + 1
        elif c == "," and depth == 1:
            args.append("".join(cur).strip())
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    return None, n


def _unquote(a):
    return a[1:-1] if len(a) >= 2 and a[0] == '"' and a[-1] == '"' else None


def parse_def(path):
    """Return (rows, errors)."""
    errors, rows, seen = [], [], {}
    with open(path, encoding="utf-8") as fh:
        text = _strip_comments(fh.read())
    for m in re.finditer(r"\bMH_INI_KEY\s*\(", text):
        line = text.count("\n", 0, m.start()) + 1
        args, _ = split_args(text, m.end())
        where = "%s:%d" % (_rel(path), line)
        if args is None or len(args) != 9:
            errors.append("%s: MH_INI_KEY needs exactly 9 columns" % where)
            continue
        sec, key, typ, dflt, cls, label, sub, opts, flg = args
        r = Row()
        r.line = line
        r.section, r.key = _unquote(sec), _unquote(key)
        r.default, r.label, r.options = _unquote(dflt), _unquote(label), _unquote(opts)
        if None in (r.section, r.key, r.default, r.label, r.options):
            errors.append("%s: section/key/default/label/options must be string literals" % where)
            continue
        if typ not in TYPES:
            errors.append("%s: unknown type %s" % (where, typ))
            continue
        if cls not in CLASSES:
            errors.append("%s: unknown class %s" % (where, cls))
            continue
        if sub not in SUBTABS:
            errors.append("%s: unknown sub-tab %s" % (where, sub))
            continue
        fl = [f.strip() for f in flg.split("|")]
        bad = [f for f in fl if f not in FLAGS]
        if bad:
            errors.append("%s: unknown flag %s" % (where, ",".join(bad)))
            continue
        r.type, r.cls, r.subtab = TYPES[typ], CLASSES[cls], SUBTABS[sub]
        r.lo = r.hi = None
        r.flags = sorted(FLAGS[f] for f in fl if FLAGS[f])
        if r.ident() in seen:
            errors.append(
                "%s: duplicate [%s] %s (first at line %d)"
                % (where, r.section, r.key, seen[r.ident()])
            )
            continue
        seen[r.ident()] = line
        errors += [where + ": " + e for e in validate_row(r)]
        rows.append(r)
    if not rows and not errors:
        errors.append("%s: no MH_INI_KEY rows found" % _rel(path))
    return rows, errors


def validate_row(r):
    e = []
    if r.type == "bool" and r.default not in ("0", "1"):
        e.append("[%s] %s: bool default must be 0 or 1" % (r.section, r.key))
    if r.type == "int" and not re.fullmatch(r"-?\d+", r.default):
        e.append("[%s] %s: int default must be a decimal number" % (r.section, r.key))
    if r.type == "float":
        num = r"(-?[0-9]+(?:\.[0-9]*)?(?:[eE][-+]?[0-9]+)?)"
        m = re.fullmatch(r"\s*" + num + r"\s*\.\.\s*" + num + r"\s*", r.options or "")
        if m:
            r.lo, r.hi = float(m.group(1)), float(m.group(2))
        if not m or not r.lo <= r.hi:
            e.append('[%s] %s: float needs a valid options range "lo..hi"' % (r.section, r.key))
        else:
            try:
                if not r.lo <= float(r.default) <= r.hi:
                    e.append("[%s] %s: float default outside its range" % (r.section, r.key))
            except ValueError:
                e.append("[%s] %s: float default must be a number" % (r.section, r.key))
    if r.subtab == "gameplay" and "host" not in r.flags:
        e.append("[%s] %s: a gameplay sub-tab row requires F_HOSTSYNC" % (r.section, r.key))
    if "host" in r.flags and r.type not in ("bool", "int", "float"):
        e.append("[%s] %s: F_HOSTSYNC requires a bool/int/float row" % (r.section, r.key))
    if r.type == "enum":
        opts = r.options.split("|") if r.options else []
        if not opts:
            e.append("[%s] %s: enum needs options" % (r.section, r.key))
        elif r.default not in opts and not (r.cls != "user" and r.default == ""):
            e.append("[%s] %s: default %r is not among the options" % (r.section, r.key, r.default))
    if r.cls == "user":
        if not r.label:
            e.append("[%s] %s: a user row needs a label id" % (r.section, r.key))
        if r.subtab is None:
            e.append("[%s] %s: a user row needs a sub-tab" % (r.section, r.key))
    else:
        if r.label or r.subtab is not None:
            e.append("[%s] %s: only user rows carry a label / sub-tab" % (r.section, r.key))
        if "experimental" in r.flags:
            e.append("[%s] %s: only user rows can be experimental" % (r.section, r.key))
    return e


# --------------------------------------------------------------------------------- code scan ----


def scan_reads(src_dir):
    """Return (reads, unresolved): reads[(section, key)] = [(relpath, line)]; unresolved is a list
    of (relpath, line, section_arg, key_arg) for call sites that are not literal and not DYNAMIC_OK."""
    reads, unresolved = {}, []
    for root, dirs, files in os.walk(src_dir):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for fn in files:
            if not fn.endswith(SOURCE_EXT):
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, src_dir).replace("\\", "/")
            with open(path, encoding="utf-8", errors="replace") as fh:
                text = _strip_comments(fh.read())
            for m in READER_RE.finditer(text):
                args, _ = split_args(text, m.end())
                if not args:
                    continue
                # A declaration/definition (`int ini_int(const char *section, ...)`) is not a read.
                if (
                    re.search(r"\bconst\b|\bchar\b|\bint\b\s*\w+\s*$", args[0])
                    or "const" in args[0]
                ):
                    continue
                spec = READERS[m.group(1)]
                try:
                    sec_a = spec[0] if isinstance(spec[0], str) else args[spec[0]]
                    key_a = args[spec[1]]
                except IndexError:
                    continue
                if isinstance(spec[0], str):
                    sec_a = '"%s"' % spec[0]
                line = text.count("\n", 0, m.start()) + 1
                sec, key = _unquote(sec_a), _unquote(key_a)
                if sec is not None and key is not None:
                    if any(rel.endswith(a) and sec == b and key == c for a, b, c in OTHER_FILE_OK):
                        continue
                    reads.setdefault((sec, key), []).append((rel, line))
                elif not any(
                    rel.endswith(a) and sec_a == b and key_a == c for a, b, c in DYNAMIC_OK
                ):
                    unresolved.append((rel, line, sec_a, key_a))
    return reads, unresolved


# ------------------------------------------------------------------------------ example ini -----

SECTION_RE = re.compile(r"^\s*;?\s*\[([A-Za-z_][A-Za-z_0-9]*)\]\s*(?:--.*)?$")
KEY_RE = re.compile(r"^(?:;\s?)?([A-Za-z_][A-Za-z_0-9.]*)\s*=")


def ini_keys(path):
    """Keys named in the example ini: live `k=v` and the narrow commented form `;k=v` / `; k=v`,
    each under the most recent LIVE `[section]` header. Returns {(section, key): first_line}."""
    out, section = {}, None
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().split("\n")
    for i, line in enumerate(lines, 1):
        s = re.match(r"^\[([A-Za-z_][A-Za-z_0-9]*)\]\s*$", line)
        if s:
            section = s.group(1)
            continue
        if section is None:
            continue
        m = KEY_RE.match(line)
        if m:
            out.setdefault((section, m.group(1)), i)
    return out


# ------------------------------------------------------------------------------------ schema ----


def build_schema(rows):
    users = [r for r in rows if r.cls == "user"]
    settings = []
    for r in sorted(users, key=lambda r: SUBTAB_ORDER.index(r.subtab)):  # stable: .def order within
        if r.type == "enum":
            options = r.options.split("|")
        elif r.type == "int" and r.options:
            options = [int(x) for x in r.options.split("|")]
        else:
            options = []
        d = int(r.default) if r.type in ("int", "bool") else r.default
        if r.type == "float":
            d = float(r.default)
        ent = {
            "section": r.section,
            "key": r.key,
            "type": r.type,
            "default": d,
            "label": r.label,
            "subtab": r.subtab,
            "options": options,
            "experimental": "experimental" in r.flags,
            "restart": "restart" in r.flags,
            "pending": "pending" in r.flags,
        }
        if r.type == "float":
            ent["min"], ent["max"] = r.lo, r.hi
        if "host" in r.flags:
            ent["host"] = True
        settings.append(ent)
    return {
        "_generated": "tools/gen_ini_registry.py from src/mh_dll/mh/config/ini_keys.def -- do not hand-edit",
        "schema_version": 1,
        "subtabs": [{"id": t, "label": "subtab." + t} for t in SUBTAB_ORDER],
        "settings": settings,
    }


def schema_text(rows):
    return json.dumps(build_schema(rows), indent=2, ensure_ascii=False) + "\n"


# -------------------------------------------------------------------------------------- check ----


def _rel(path):
    try:
        return os.path.relpath(path, REPO).replace("\\", "/")
    except ValueError:
        return path.replace("\\", "/")


def hook_problems(rows, src_dir):
    """Every F_HOSTSYNC row has exactly one hook entry in gameplay_hooks.cpp and vice versa. Vacuous
    with no F_HOSTSYNC rows and no hooks file."""
    hooks = {}
    hp = os.path.join(src_dir, HOOKS_FILE)
    if os.path.exists(hp):
        with open(hp, encoding="utf-8", errors="replace") as fh:
            text = _strip_comments(fh.read())
        for m in HOOK_RE.finditer(text):
            hooks.setdefault((m.group(1), m.group(2)), []).append(
                text.count("\n", 0, m.start()) + 1
            )
    out = []
    reg = {r.ident(): r for r in rows}
    for r in rows:
        if "host" in r.flags:
            n = len(hooks.get(r.ident(), []))
            if n != 1:
                out.append(
                    "%s: [%s] %s is F_HOSTSYNC but has %d hook entries (need exactly 1)"
                    % (HOOKS_FILE, r.section, r.key, n)
                )
    for ident, lines in sorted(hooks.items()):
        if ident not in reg or "host" not in reg[ident].flags:
            out.append(
                "%s:%d: hook for [%s] %s has no F_HOSTSYNC registry row (orphan hook)"
                % (HOOKS_FILE, lines[0], ident[0], ident[1])
            )
    return out


def check(def_path, src_dir, ini_path, schema_path, say=print):
    rows, errs = parse_def(def_path)
    problems = list(errs)
    reads, unresolved = scan_reads(src_dir)
    reg = {r.ident(): r for r in rows}

    for (sec, key), where in sorted(reads.items()):
        if (sec, key) not in reg:
            problems.append(
                "%s:%d: [%s] %s is read but has no row in %s -- add one (class dev unless the "
                "plan's settings table says otherwise)"
                % (where[0][0], where[0][1], sec, key, _rel(def_path))
            )
    for (sec, key), ws in sorted(reads.items()):
        if (sec, key) in reg and "host" in reg[(sec, key)].flags:
            for rel_, line_ in ws:
                if not rel_.endswith(GAMEPLAY_HEADER):
                    problems.append(
                        "%s:%d: [%s] %s is F_HOSTSYNC -- read it through mh_gameplay.h, never directly"
                        % (rel_, line_, sec, key)
                    )
    problems += hook_problems(rows, src_dir)
    for r in rows:
        if r.ident() not in reads and "pending" not in r.flags and "host" not in r.flags:
            problems.append(
                "%s:%d: [%s] %s is registered but nothing reads it -- delete the row (or flag it "
                "F_PENDING if its reader lands later)" % (_rel(def_path), r.line, r.section, r.key)
            )
    for rel, line, sec_a, key_a in unresolved:
        problems.append(
            "%s:%d: ini read with a non-literal section/key (%s, %s) -- the registry cannot see it; "
            "use literals, or add the wrapper to DYNAMIC_OK in tools/gen_ini_registry.py"
            % (rel, line, sec_a, key_a)
        )

    if os.path.exists(ini_path):
        named = ini_keys(ini_path)
        for (sec, key), line in sorted(named.items(), key=lambda kv: kv[1]):
            if (sec, key) not in reg and not any(
                s == sec and rx.match(key) for s, rx in INI_DYNAMIC
            ):
                problems.append(
                    "%s:%d: [%s] %s is named in the example ini but is not in the registry"
                    % (_rel(ini_path), line, sec, key)
                )
        for r in rows:
            if r.cls in ("user", "fixed") and r.ident() not in named and "pending" not in r.flags:
                problems.append(
                    "%s: %s key [%s] %s is not documented in the example ini"
                    % (_rel(ini_path), r.cls, r.section, r.key)
                )
    else:
        problems.append("%s: example ini missing" % _rel(ini_path))

    if not errs:
        want = schema_text(rows)
        have = None
        if os.path.exists(schema_path):
            with open(schema_path, encoding="utf-8", newline="") as fh:
                have = fh.read().replace("\r\n", "\n")
        if have != want:
            problems.append(
                "%s is %s -- run `python tools/gen_ini_registry.py --write`"
                % (_rel(schema_path), "missing" if have is None else "stale")
            )

    for p in problems:
        say(p)
    if problems:
        say("gen_ini_registry: FAIL -- %d problem(s)" % len(problems))
        return 1
    pend = [r for r in rows if "pending" in r.flags]
    counts = {c: sum(1 for r in rows if r.cls == c) for c in ("user", "dev", "fixed")}
    say(
        "gen_ini_registry: ok -- %d keys (%d user, %d dev, %d fixed), %d read sites in code; "
        "pending (no reader yet): %s"
        % (
            len(rows),
            counts["user"],
            counts["dev"],
            counts["fixed"],
            sum(len(v) for v in reads.values()),
            ", ".join("[%s] %s" % (r.section, r.key) for r in pend) or "none",
        )
    )
    return 0


def write_schema(def_path, schema_path, quiet=False):
    rows, errs = parse_def(def_path)
    if errs:
        for e in errs:
            print(e)
        return 1
    os.makedirs(os.path.dirname(schema_path), exist_ok=True)
    with open(schema_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(schema_text(rows))
    if not quiet:
        print(
            "wrote %s (%d user settings)" % (_rel(schema_path), sum(r.cls == "user" for r in rows))
        )
    return 0


# ------------------------------------------------------------------------------------ selftest ---

_DEF_OK = (
    '// header\nMH_INI_KEY("video", "vsync", T_BOOL, "0", C_USER, "video.vsync", S_DISPLAY, "", F_NONE)\n'
    'MH_INI_KEY("net", "peers", T_INT, "1", C_DEV, "", S_NONE, "", F_NONE)\n'
    'MH_INI_KEY("log", "level", T_ENUM, "normal", C_USER, "log.level", S_DIAGNOSTICS, '
    '"quiet|normal|debug", F_PENDING)\n'
)
_SRC_OK = (
    "void f(const char *ini) {\n"
    '  GetPrivateProfileIntA("video", "vsync", 0, ini);\n'
    '  GetPrivateProfileIntA("net", "peers", 1, ini);\n'
    "}\n"
)
_INI_OK = "[video]\nvsync=0\n[net]\n;peers=1\n"


def selftest():
    fails = []

    def expect(name, cond):
        if not cond:
            fails.append(name)
        print(("ok   " if cond else "FAIL ") + name)

    tmp = tempfile.mkdtemp(prefix="ini_reg_")
    try:
        src = os.path.join(tmp, "src")
        os.makedirs(src)
        d, c, i, s = (
            os.path.join(tmp, x)
            for x in ("k.def", os.path.join("src", "a.cpp"), "x.ini", "schema.json")
        )

        def put(path, text):
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)

        def run(def_t=_DEF_OK, src_t=_SRC_OK, ini_t=_INI_OK):
            put(d, def_t)
            put(c, src_t)
            put(i, ini_t)
            write_schema(d, s, quiet=True)
            out = []
            rc = check(d, src, i, s, say=out.append)
            return rc, "\n".join(out)

        rc, out = run()
        expect("a consistent registry/code/ini triple is green", rc == 0)
        expect("a F_PENDING row with no reader is not demanded", rc == 0 and "[log] level" in out)

        rc, out = run(src_t=_SRC_OK + '  GetPrivateProfileIntA("net", "planted", 0, ini);\n')
        expect(
            "a planted UNREGISTERED key is RED and named",
            rc == 1 and "[net] planted is read but has no row" in out,
        )
        rc, out = run(
            src_t=_SRC_OK + '  mh::config::read_ini_string("net", "planted_s", "", b, 8, ini);\n'
        )
        expect("a planted unregistered read_ini_string key is RED", rc == 1 and "planted_s" in out)
        rc, out = run(src_t=_SRC_OK + '  net_ini_int("planted_w", 0);\n')
        expect(
            "a planted unregistered wrapper (net_ini_int) key is RED",
            rc == 1 and "planted_w" in out,
        )
        rc, out = run(src_t=_SRC_OK + '  // GetPrivateProfileIntA("net", "in_comment", 0, ini);\n')
        expect("a read inside a comment is not a read", rc == 0)
        rc, out = run(src_t=_SRC_OK + "  GetPrivateProfileIntA(sec, k, 0, ini);\n")
        expect("a non-literal read site is RED", rc == 1 and "non-literal" in out)
        rc, out = run(
            src_t=_SRC_OK.replace('  GetPrivateProfileIntA("net", "peers", 1, ini);\n', "")
        )
        expect("a registered key nothing reads is RED", rc == 1 and "nothing reads it" in out)
        rc, out = run(ini_t=_INI_OK + "[net]\nghost=1\n")
        expect("an unregistered key in the example ini is RED", rc == 1 and "ghost" in out)
        rc, out = run(ini_t="[net]\n;peers=1\n")
        expect(
            "a user key missing from the example ini is RED", rc == 1 and "not documented" in out
        )
        rc, out = run(def_t=_DEF_OK + _DEF_OK.split("\n")[1] + "\n")
        expect("a duplicate registry row is RED", rc == 1 and "duplicate" in out)
        rc, out = run(
            def_t=_DEF_OK.replace('"video.vsync", S_DISPLAY', '"", S_NONE'),
        )
        expect("a user row without label/sub-tab is RED", rc == 1 and "needs a label" in out)
        rc, out = run(def_t=_DEF_OK.replace("T_BOOL", "T_BOGUS"))
        expect("an unknown enumerator is RED", rc == 1 and "unknown type" in out)
        # PT-SET1 plants.
        hdef = (
            _DEF_OK
            + 'MH_INI_KEY("net", "gp_mult", T_FLOAT, "2.0", C_USER, "net.gp_mult", S_GAMEPLAY, '
            '"0.1..10", F_HOSTSYNC)\n'
        )
        hook = os.path.join(src, "mh", "seams", "gameplay_hooks.cpp")
        os.makedirs(os.path.dirname(hook), exist_ok=True)
        hsrc = os.path.join(src, "mh_common", "include", "mh_gameplay.h")
        os.makedirs(os.path.dirname(hsrc), exist_ok=True)
        put(hsrc, "void g(){ mh_ini_get_str(r.sec, r.key, 0); }\n")
        good_hook = '{"net","gp_mult",[](const V &v, F &fx){ fx.x = 1; }},\n'
        put(hook, good_hook)
        rc, out = run(def_t=hdef, ini_t=_INI_OK + "[net]\ngp_mult=2.0\n")
        expect("a hostsync float row with its hook is green", rc == 0)
        os.remove(hook)
        rc, out = run(def_t=hdef, ini_t=_INI_OK + "[net]\ngp_mult=2.0\n")
        expect("a hostsync row with a MISSING hook is RED", rc == 1 and "0 hook entries" in out)
        put(hook, good_hook + '{"net","ghost",[](const V &v, F &fx){}},\n')
        rc, out = run(def_t=hdef, ini_t=_INI_OK + "[net]\ngp_mult=2.0\n")
        expect("an ORPHAN hook is RED", rc == 1 and "orphan hook" in out)
        put(hook, good_hook)
        rc, out = run(
            def_t=hdef,
            src_t=_SRC_OK + '  GetPrivateProfileStringA("net", "gp_mult", "", b, 8, ini);\n',
            ini_t=_INI_OK + "[net]\ngp_mult=2.0\n",
        )
        expect("a DIRECT read of a hostsync key is RED", rc == 1 and "never directly" in out)
        os.remove(hook)
        os.remove(hsrc)
        rc, out = run(def_t=hdef.replace('"0.1..10"', '""'))
        expect("a float row without a range is RED", rc == 1 and "valid options range" in out)
        rc, out = run(def_t=hdef.replace('"0.1..10"', '"5..1"'))
        expect(
            "a float row with an inverted range is RED", rc == 1 and "valid options range" in out
        )
        rc, out = run(def_t=hdef.replace("F_HOSTSYNC", "F_NONE"))
        expect("S_GAMEPLAY without F_HOSTSYNC is RED", rc == 1 and "requires F_HOSTSYNC" in out)
        rc, out = run(
            def_t=_DEF_OK
            + 'MH_INI_KEY("net", "s", T_STRING, "", C_DEV, "", S_NONE, "", F_HOSTSYNC)\n'
        )
        expect("F_HOSTSYNC on a string row is RED", rc == 1 and "bool/int/float" in out)
        # Stale schema: change the committed file after generating it.
        run()
        put(s, "{}\n")
        out = []
        expect("a stale schema JSON is RED", check(d, src, i, s, say=out.append) == 1)
    finally:
        import shutil

        shutil.rmtree(tmp, ignore_errors=True)

    # The committed tree itself must pass: a selftest green over a red gate would be the vacuous kind.
    expect(
        "the committed registry passes --check",
        check(DEFAULT_DEF, DEFAULT_SRC, DEFAULT_INI, DEFAULT_SCHEMA, say=lambda *a: None) == 0,
    )
    print("gen_ini_registry selftest: %d failure(s)" % len(fails))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--def", dest="def_path", default=DEFAULT_DEF)
    ap.add_argument("--src", default=DEFAULT_SRC)
    ap.add_argument("--ini", default=DEFAULT_INI)
    ap.add_argument("--schema", default=DEFAULT_SCHEMA)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--check", action="store_true", help="the lint (default)")
    g.add_argument("--write", action="store_true", help="regenerate the launcher schema JSON")
    g.add_argument("--list", action="store_true")
    g.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.write:
        return write_schema(a.def_path, a.schema)
    if a.list:
        rows, errs = parse_def(a.def_path)
        for e in errs:
            print(e)
        for cls in ("user", "fixed", "dev"):
            sel = [r for r in rows if r.cls == cls]
            print("== %s (%d)" % (cls, len(sel)))
            for r in sel:
                print(
                    "  [%s] %s  %s default=%r %s"
                    % (r.section, r.key, r.type, r.default, ",".join(r.flags))
                )
        return 1 if errs else 0
    return check(a.def_path, a.src, a.ini, a.schema)


if __name__ == "__main__":
    sys.exit(main())
