#!/usr/bin/env python3
"""Guard tools/requirements.txt against rot: every third-party package tools/**.py imports must be
pinned there.

Why this exists: none of tools/'s dependencies were declared anywhere, so a fresh machine could not
install what tools/ needs without reading every script (bootstrap item E1). The manifest fixes that
only if it stays complete -- and a hand-maintained dependency list rots the first time someone adds
an `import` without updating it. So the manifest is DERIVED and this check re-derives it:

  1. AST-walk every tools/**.py, collect top-level import names.
  2. Drop the stdlib, sibling tools modules (local imports), and RUNTIME_PROVIDED (the JVM-only
     packages that are not pip-installable and correctly absent from the manifest).
  3. Map each remaining import name to its installed distribution (importlib.metadata, with a small
     static fallback for the names whose distribution differs from the import: PIL->Pillow etc.).
  4. FAIL if any such distribution is not pinned in tools/requirements.txt.

Runs inside lint_repo.py, so it must be hermetic: it only READS the tree + the current environment's
package metadata; it writes nothing.

Usage:
  python tools/lint_requirements.py           # check; exit 1 if the manifest is missing a package
  python tools/lint_requirements.py --list     # print the derived import -> distribution table
"""

import argparse
import ast
import json
import os
import sys
from pathlib import Path

from importlib import metadata as importlib_metadata

REPO = Path(__file__).resolve().parent.parent
TOOLS = REPO / "tools"
REQUIREMENTS = TOOLS / "requirements.txt"

# Packages that resolve only inside a running Ghidra/PyGhidra JVM -- not on PyPI, so deliberately not
# in the manifest. If one of these ever gains a pip distribution, move it to the manifest.
RUNTIME_PROVIDED = {"ghidra", "java", "reva", "docking", "generic", "db", "utility"}

# Fallback import-name -> distribution-name for names that differ from their import. Primary source is
# importlib.metadata.packages_distributions() on the installed env; this covers the case where the
# package is not installed on the machine running the check (so the derived table is still meaningful).
STATIC_IMPORT_TO_DIST = {
    "PIL": "Pillow",
    "yaml": "PyYAML",
    "jpype": "JPype1",
    "keystone": "keystone-engine",
    "cv2": "opencv-python",
}


def local_module_names():
    """Every first-party .py stem in the repo -- sibling/`src` imports, not external packages.
    Scanned repo-wide (not just tools/) because tools scripts sys.path-insert and import first-party
    modules from src/ (adopt_sprite, decompress, mhpatch).

    THE DISPOSITION ROSTER IS UNIONED IN (fork F5M S4b), and the reason is a defect this lint
    actually produced: the archive-tool cut withholds 80 top-level tools/*.py from the public tree,
    and a LAZY import of one of them inside a carried module then looked like an unpinned PyPI
    distribution -- `migration_ledger` and `migration_seeds` were reported as missing pins on a
    freshly materialized public clone. They are not packages and no pin could ever exist. The
    roster in tools/data/tool_dispositions.json names every top-level tool the REPO has, present or
    withheld, so unioning it keeps "is this first-party?" a question about the project rather than
    about which files this particular checkout carries. Absent or unreadable, the on-disk scan
    stands alone -- exactly the behaviour before this change."""
    stems = set()
    for sub in ("tools", "src"):
        d = REPO / sub
        if d.exists():
            stems |= {p.stem for p in d.rglob("*.py")}
    try:
        with open(REPO / "tools" / "data" / "tool_dispositions.json", encoding="utf-8") as fh:
            stems |= {t[:-3] for t in json.load(fh)["rows"] if t.endswith(".py")}
    except (OSError, ValueError, KeyError):
        pass
    return stems


def third_party_imports():
    """{import_name: sorted[files]} for every non-stdlib, non-local, non-runtime top-level import."""
    stdlib = set(sys.stdlib_module_names)
    local = local_module_names()
    found: dict[str, set[str]] = {}
    for p in sorted(TOOLS.rglob("*.py")):
        try:
            tree = ast.parse(p.read_text(encoding="utf-8", errors="replace"))
        except SyntaxError:
            continue
        tops = set()
        for n in ast.walk(tree):
            if isinstance(n, ast.Import):
                for a in n.names:
                    tops.add(a.name.split(".")[0])
            elif isinstance(n, ast.ImportFrom):
                if n.level == 0 and n.module:
                    tops.add(n.module.split(".")[0])
        for t in tops:
            if t in stdlib or t in local or t in RUNTIME_PROVIDED:
                continue
            found.setdefault(t, set()).add(p.name)
    return {k: sorted(v) for k, v in found.items()}


def import_to_distributions():
    """Map top-level import name -> [distribution names], from the installed env."""
    try:
        return importlib_metadata.packages_distributions()
    except Exception:
        return {}


def resolve_dist(import_name, dist_map):
    """Best-effort distribution name for an import: installed metadata, then static fallback, then
    the import name itself (many packages have matching names, e.g. numpy, lief, tqdm, capstone)."""
    if import_name in dist_map and dist_map[import_name]:
        return dist_map[import_name][0]
    if import_name in STATIC_IMPORT_TO_DIST:
        return STATIC_IMPORT_TO_DIST[import_name]
    return import_name


def parse_pinned():
    """Distribution names (lowercased, normalized) pinned in requirements.txt."""
    pinned = {}
    if not REQUIREMENTS.exists():
        return pinned
    for line in REQUIREMENTS.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        # split off version specifier
        name = line
        for sep in ("==", ">=", "<=", "~=", ">", "<", "!=", "["):
            if sep in name:
                name = name.split(sep)[0]
        name = name.strip()
        if name:
            pinned[_norm(name)] = line
    return pinned


def _norm(dist_name):
    # PEP 503 normalization: lowercase, runs of -_. collapse to a single -
    import re

    return re.sub(r"[-_.]+", "-", dist_name).lower()


def check(verbose=False):
    imports = third_party_imports()
    dist_map = import_to_distributions()
    pinned = parse_pinned()

    rows = []
    missing = []
    for imp in sorted(imports):
        dist = resolve_dist(imp, dist_map)
        ok = _norm(dist) in pinned
        rows.append((imp, dist, ok, imports[imp]))
        if not ok:
            missing.append((imp, dist, imports[imp]))

    if verbose:
        print(f"{'import':14} {'distribution':22} pinned  used-by")
        for imp, dist, ok, files in rows:
            mark = "yes" if ok else "NO "
            print(f"{imp:14} {dist:22} {mark:6}  {', '.join(files[:4])}")
        print(f"\nruntime-provided (not pip): {', '.join(sorted(RUNTIME_PROVIDED))}")

    if missing:
        print(
            "lint_requirements: FAIL -- imports not pinned in tools/requirements.txt:",
            file=sys.stderr,
        )
        for imp, dist, files in missing:
            print(
                f"  - `{imp}` (distribution `{dist}`) imported by {', '.join(files)}",
                file=sys.stderr,
            )
        print(
            "  Fix: add the distribution (with a version pin) to tools/requirements.txt, or -- if it is\n"
            "  JVM-runtime-provided and not on PyPI -- add its import name to RUNTIME_PROVIDED here.",
            file=sys.stderr,
        )
        return 1
    if not verbose:
        print(f"lint_requirements: OK ({len(rows)} third-party imports, all pinned)")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--list", action="store_true", help="print the derived import->distribution table"
    )
    args = ap.parse_args()
    sys.exit(check(verbose=args.list))


if __name__ == "__main__":
    # tools/ shadows the JVM `ghidra` package for pyghidra tools, but this script imports none, so the
    # sys.path[0] tools-dir entry is harmless here.
    main()
