#!/usr/bin/env python3
"""lint_resources.py -- tooling:TL-SUITE-RESREG. Refuse an unregistered machine-global resource.

12+ separate incidents (TL-RIG6/7/8/11, TL-LOCKRACE, TL-POLITELOCK, TL-LANECOLLIDE, F4H lane
collision, TL-SHIMCTL, TL-SELFTEST-STAGE, TL-TMPLEAK, G268, hostlock reaping a live holder) were all
the same shape: a literal port / %TEMP% path / lock name / mutex name that TWO forms of parallelism
(a second worktree, a second lane, a second agent) both bind, fixed one incident at a time. This scans
`tools/*.py` and `tools/*.bat` (recursively) for such literals with plain regex heuristics --
bind()/connect() calls with an inline literal, a NAME=<int> assignment where NAME looks like a port, a
tuple of 4-5-digit literals assigned together (`ECHO, LISTEN, CTL = 39710, 39711, 39799`), an
argparse `--*port*` default, a `port=<int>` occurrence inside a string literal (ini text / batch `SET`),
a %TEMP%-rooted path literal, a `*.lock` file-name literal, or an `MHMut*` mutex literal -- and refuses
any that tools/data/resource_registry.json does not name. This is a coverage gate, not a network
scanner: it never binds anything, it only reads source text.

Usage:
    python tools/lint_resources.py             # scan tools/, refuse on any uncovered literal
    python tools/lint_resources.py --selftest  # plant two negatives, prove both get caught

Deliberately NOT exhaustive Python parsing (this project's other lints use plain regex/text scans over
`tools/*.py` for the same reason -- see lint_repo.py's own rows): a resource literal is meant to be
easy to spot by eye in a diff, so a regex that matches how a human would write one is the right
strength, not an AST walk that would also have to model f-strings, `.format()` and `%`-formatting to
find "port=6501" wherever it hides.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ROOT = os.path.join(REPO, "tools")
REGISTRY_PATH = os.path.join(REPO, "tools", "data", "resource_registry.json")

PORT_MIN, PORT_MAX = 1024, 65535

# ---- literal detectors ----------------------------------------------------------------------------
# Each returns an iterable of (line_no, kind, literal) for one physical line (or a short joined
# window, for the argparse case, which spans several lines in this codebase's style).

_PORT_ASSIGN_RE = re.compile(
    r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?::\s*int)?\s*=\s*(\d{4,5})\s*(?:#.*)?$"
)
_TUPLE_INT_RE = re.compile(r"\d{4,5}")
_BIND_CALL_RE = re.compile(r"\.(?:bind|connect)\s*\(\s*\([^()]*?,\s*(\d{4,5})\s*\)")
_ARGPARSE_PORT_RE = re.compile(
    r"add_argument\(\s*[\"'][^\"']*?port[^\"']*?[\"'][\s\S]{0,200}?default\s*=\s*(\d{4,5})",
    re.IGNORECASE,
)
_INI_PORT_RE = re.compile(r"\bport\b\s*=\s*(\d{4,5})\b", re.IGNORECASE)
_TEMP_DIR_RE = re.compile(r"(?:%TEMP%\\+|TEMP\s*,\s*[\"'])([A-Za-z0-9_.]+)")
_LOCK_NAME_RE = re.compile(r"[\"']([A-Za-z0-9_./\\-]*\.lock)[\"']")
_MUTEX_RE = re.compile(r"[\"'](MHMut[A-Za-z0-9%]*)[\"']")
# Well-known lock-file names that belong to an external ecosystem (Cargo, npm, ...), not to this
# project's own resource space -- referencing one (e.g. to read/remove a stray Cargo.lock) is not
# the "two agents fight over one machine-global name" hazard this lint exists for.
_LOCK_NAME_ALLOW = {"Cargo.lock"}


def _port_name_hints(name: str) -> bool:
    return "port" in name.lower()


def scan_text(path: str, text: str):
    """Return a list of (line_no, kind, literal_str) for every candidate literal in `text`."""
    lines = text.splitlines()
    seen = set()
    found = []

    def emit(line_no, kind, literal):
        key = (line_no, kind, literal)
        if key not in seen:
            seen.add(key)
            found.append((line_no, kind, literal))

    for i, line in enumerate(lines, start=1):
        # 1. NAME = <int> where NAME looks like a port
        m = _PORT_ASSIGN_RE.match(line)
        if m:
            name, num = m.group(1), int(m.group(2))
            if PORT_MIN <= num <= PORT_MAX and _port_name_hints(name):
                emit(i, "port", str(num))
        # 2. a tuple of 2+ port-range literals assigned to ALL-CAPS names on one line, e.g.
        #    `ECHO, LISTEN, CTL = 39710, 39711, 39799` -- narrow on purpose (ALL-CAPS identifiers
        #    only) so it does not fire on ordinary numeric test fixtures/log constants.
        m2 = re.match(r"^\s*([A-Z_][A-Z0-9_]*(?:\s*,\s*[A-Z_][A-Z0-9_]*)+)\s*=\s*(.+)$", line)
        if m2:
            names = [n.strip() for n in m2.group(1).split(",")]
            nums = _TUPLE_INT_RE.findall(m2.group(2))
            in_range = [n for n in nums if PORT_MIN <= int(n) <= PORT_MAX]
            if len(names) >= 2 and len(in_range) >= 2 and len(nums) == len(names):
                for n in in_range:
                    emit(i, "port", n)
        # 3. bind()/connect() with an inline literal port
        for m in _BIND_CALL_RE.finditer(line):
            emit(i, "port", m.group(1))
        # 4. port=<int> inside a string literal (ini text, batch SET, docstrings)
        for m in _INI_PORT_RE.finditer(line):
            emit(i, "port", m.group(1))
        # 5. a %TEMP%-rooted fixed directory name
        for m in _TEMP_DIR_RE.finditer(line):
            name = m.group(1)
            if name.upper() not in ("TEMP",):
                emit(i, "temp_dir", name)
        # 6. a *.lock file-name literal (excluding well-known external-ecosystem lock files)
        for m in _LOCK_NAME_RE.finditer(line):
            if m.group(1) not in _LOCK_NAME_ALLOW:
                emit(i, "lock_name", m.group(1))
        # 7. an MHMut* mutex literal
        for m in _MUTEX_RE.finditer(line):
            emit(i, "mutex", m.group(1))

    # 8. argparse --*port* default, joined over a short window (multi-line add_argument calls)
    for m in re.finditer(r"add_argument\(", text):
        window = text[m.start() : m.start() + 400]
        am = _ARGPARSE_PORT_RE.match(window)
        if am:
            line_no = text.count("\n", 0, m.start()) + 1
            emit(line_no, "port", am.group(1))

    return found


# ---- registry ---------------------------------------------------------------------------------


def load_registry(path: str = REGISTRY_PATH):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def _numbers_in(value) -> list[int]:
    return [int(n) for n in re.findall(r"\d{4,5}", str(value))]


class Coverage:
    """What resource_registry.json says is accounted for, indexed for a fast lookup per kind."""

    def __init__(self, registry: dict):
        self.ports: set[int] = set()
        self.port_ranges: list[tuple[int, int]] = []
        self.temp_dirs: set[str] = set()
        self.lock_names: set[str] = set()
        self.mutex_prefixes: list[str] = []
        for res in registry.get("resources", []):
            kind = res.get("kind", "")
            value = str(res.get("value", ""))
            if "port" in kind:
                # an explicit A-B range in the value string
                for lo, hi in re.findall(r"(\d{4,5})\s*-\s*(\d{4,5})", value):
                    self.port_ranges.append((int(lo), int(hi)))
                # a "<base> + n" / "<base> + test_index" band -- this codebase's bands are all
                # PORT_BAND=100 wide (tools/lane_alloc.py); treat the base the same way.
                for base in re.findall(r"(\d{4,5})\s*\+\s*[A-Za-z_]", value):
                    b = int(base)
                    self.port_ranges.append((b, b + 99))
                for n in _numbers_in(value):
                    self.ports.add(n)
            elif kind == "temp_dir":
                for name in re.findall(r"mh_[A-Za-z0-9_]+", value):
                    self.temp_dirs.add(name)
            elif kind == "lock_file":
                for name in re.findall(r"[\w.]*\.lock", value):
                    self.lock_names.add(name)
                self.lock_names.add(os.path.basename(value.split(" ")[0]).strip("<>"))
            elif kind == "mutex_name":
                self.mutex_prefixes.append("MHMut")

    def port_ok(self, n: int) -> bool:
        if n in self.ports:
            return True
        return any(lo <= n <= hi for lo, hi in self.port_ranges)

    def temp_dir_ok(self, name: str) -> bool:
        return name in self.temp_dirs

    def lock_name_ok(self, name: str) -> bool:
        base = os.path.basename(name.replace("\\", "/"))
        return name in self.lock_names or base in self.lock_names

    def mutex_ok(self, literal: str) -> bool:
        return any(literal.startswith(p) for p in self.mutex_prefixes)

    def ok(self, kind: str, literal: str) -> bool:
        if kind == "port":
            return self.port_ok(int(literal))
        if kind == "temp_dir":
            return self.temp_dir_ok(literal)
        if kind == "lock_name":
            return self.lock_name_ok(literal)
        if kind == "mutex":
            return self.mutex_ok(literal)
        return False


# ---- driver ------------------------------------------------------------------------------------


def iter_source_files(root: str):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in ("__pycache__", ".git")]
        for fn in filenames:
            if fn.endswith((".py", ".bat")):
                yield os.path.join(dirpath, fn)


def run_scan(root: str, registry_path: str = REGISTRY_PATH, quiet: bool = False):
    """Returns a list of (relpath, line_no, kind, literal) violations."""
    coverage = Coverage(load_registry(registry_path))
    violations = []
    self_path = os.path.abspath(__file__)
    for path in iter_source_files(root):
        if os.path.abspath(path) == self_path:
            continue  # this file's own docstring/regexes mention every literal by design
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue
        for line_no, kind, literal in scan_text(path, text):
            if not coverage.ok(kind, literal):
                try:
                    rel = os.path.relpath(path, REPO).replace("\\", "/")
                except ValueError:  # a selftest fixture on a different drive than REPO (Windows)
                    rel = path.replace("\\", "/")
                violations.append((rel, line_no, kind, literal))
    return violations


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=DEFAULT_ROOT, help="directory to scan (default: tools/)")
    ap.add_argument("--registry", default=REGISTRY_PATH)
    ap.add_argument(
        "--selftest", action="store_true", help="plant two negatives, prove both are caught"
    )
    args = ap.parse_args(argv)

    if args.selftest:
        return _selftest()

    violations = run_scan(args.root, args.registry)
    if violations:
        print(f"lint_resources: {len(violations)} unregistered resource literal(s):")
        for rel, line_no, kind, literal in sorted(violations):
            print(f"  {rel}:{line_no}: unregistered {kind} literal {literal!r}")
        print(
            "Add an entry to tools/data/resource_registry.json (name/kind/value/scope/owners/"
            "reason), or derive the literal from an existing scope (lane_alloc / a hostlock lease)."
        )
        return 1
    print("lint_resources: PASS (every literal is registered)")
    return 0


def _selftest() -> int:
    import shutil
    import tempfile

    fails = []

    def check(desc, ok):
        print(f"  [{'ok' if ok else 'XX'}] {desc}")
        if not ok:
            fails.append(desc)

    # 1. the real tree is clean at HEAD.
    real_violations = run_scan(DEFAULT_ROOT, REGISTRY_PATH)
    check(
        f"tools/ is clean against the committed registry (found {len(real_violations)})",
        not real_violations,
    )
    if real_violations:
        for rel, line_no, kind, literal in sorted(real_violations)[:20]:
            print(f"      {rel}:{line_no}: {kind} {literal!r}")

    # 2. a planted new literal port is caught.
    tmp = tempfile.mkdtemp(prefix="lint_resources_selftest_")
    try:
        with open(os.path.join(tmp, "planted_port.py"), "w", encoding="utf-8") as fh:
            fh.write('WEIRD_NEW_PORT = 54321\n\ndef f():\n    return WEIRD_NEW_PORT\n')
        violations = run_scan(tmp, REGISTRY_PATH)
        check(
            "a planted literal port (54321) is caught",
            any(k == "port" and lit == "54321" for _, _, k, lit in violations),
        )

        # 3. a planted new fixed %TEMP% dir is caught.
        with open(os.path.join(tmp, "planted_tempdir.py"), "w", encoding="utf-8") as fh:
            fh.write(
                'import os\n'
                'TEMP = os.environ.get("TEMP", "")\n'
                'EVIL_DIR = os.path.join(TEMP, "mh_evil_singleton")\n'
            )
        violations = run_scan(tmp, REGISTRY_PATH)
        check(
            "a planted fixed %TEMP% dir (mh_evil_singleton) is caught",
            any(k == "temp_dir" and lit == "mh_evil_singleton" for _, _, k, lit in violations),
        )

        # 4. a registered port (6501) in the SAME planted file is NOT flagged (no false positive).
        with open(os.path.join(tmp, "planted_ok.py"), "w", encoding="utf-8") as fh:
            fh.write('NET_GAME_PORT = 6501\n')
        violations = run_scan(tmp, REGISTRY_PATH)
        check(
            "a registered port (6501) is NOT flagged",
            not any(k == "port" and lit == "6501" for _, _, k, lit in violations),
        )
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("lint_resources selftest: " + ("PASS" if not fails else f"FAIL ({len(fails)})"))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
