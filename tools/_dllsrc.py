"""_dllsrc.py -- the DLL source trees, and the ONE spelling every scanner keys by.

WHY THIS EXISTS (fork F5O, 2026-09-16). The 628 shared-roster TUs used to live under
`src/mh_dll/mh/<domain>/`, so a scanner could take `src/mh_dll/mh` as its single walk root and
`os.path.relpath(p, MH)` as its key, and the key came out `sim/sim_state.cpp`. F5O moved the roster
to `src/mh_dll/libmh/<domain>/` -- the directory of the project that owns it -- leaving mh.dll's own
code (and the header-only trees addr/, crt/, fp/, config/, fix/, patch/, include/ plus the seams/
remainder) behind under mh/, and it moved `mh/seams/harness.cpp` to `mh_harness/`.

THREE TREES NOW, AND THEY MUST STAY ONE KEY SPACE. Every scanner that walked mh/ walks all three and
keys against WHICHEVER TREE HOLDS THE FILE, so `libmh/sim/sim_state.cpp` still keys as
`sim/sim_state.cpp` and `mh/seams/launch.cpp` still keys as `seams/launch.cpp`. That is what made
the move cheap: the module-relative spelling is what ~1500 lines of committed census data
(libmh_rebind.json, sim_liveness.json, hostapi_callers.json, ...) are keyed by, and not one of them
had to be re-keyed. Data keyed by the REPO-RELATIVE path (`src/mh_dll/mh/sim/x.cpp`, the spelling  # CITATION-OK
that names a directory rather than a module) did have to move, and did.

THE `seams/` PREFIX ON mh_harness/ IS A MEASUREMENT, NOT A COSMETIC. Every census that reads the
instrument buckets it as mh.dll's seam layer, and a file moving between DIRECTORIES must not move a
measurement between BUCKETS: the first run after the move, before this prefix existed, silently lost
22 `mh::call::` sites out of libmh_call_census.json's `seams` row because harness.cpp had left the
walked tree. So mh_harness/ keys as `seams/<file>`, which is what harness.cpp keyed as before it
moved, and the census is byte-identical across the move.

Use `module_rel(p)` rather than an open-coded relpath, `rel_dir(tree, dirpath)` rather than a bare
`os.path.relpath(root, tree)` for the module a DIRECTORY belongs to, and `walk()`/`ROOTS` rather
than a bare MH -- so the next tree that joins the split is one edit here instead of twenty.
"""

import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src", "mh_dll")

MH = os.path.join(SRC, "mh")
LIBMH = os.path.join(SRC, "libmh")
MH_HARNESS = os.path.join(SRC, "mh_harness")

#: Walk order is mh/ first, purely so a listing reads the way the tree used to.
ROOTS = (MH, LIBMH, MH_HARNESS)

#: tree -> the module path every key from that tree is prefixed with (see the header).
PREFIX = {MH: "", LIBMH: "", MH_HARNESS: "seams/"}


def module_rel(path):
    """-> the module-relative, forward-slashed key (`sim/sim_state.cpp`), or None if `path` is in
    no DLL source tree. Tree-independent BY DESIGN: see this module's header."""
    ap = os.path.abspath(path)
    for r in ROOTS:
        pre = os.path.abspath(r) + os.sep
        if ap.startswith(pre):
            return PREFIX[r] + ap[len(pre) :].replace("\\", "/")
    return None


def rel_dir(tree, dirpath):
    """-> the module-relative DIRECTORY key, in the form `os.path.relpath` used to return: "." at a
    tree root that carries no prefix, and the prefix itself at one that does."""
    rel = os.path.relpath(dirpath, tree).replace("\\", "/")
    pre = PREFIX[tree]
    if rel == ".":
        return pre.rstrip("/") or "."
    return pre + rel


def walk(roots=None):
    """os.walk over every DLL source tree, yielding (tree, dirpath, dirnames, filenames).

    `tree` is the root the row came from -- pass it back to `rel_dir` (or the file to `module_rel`)
    for the module-relative key. `dirnames` may be pruned in place exactly as with os.walk: this
    generator yields from INSIDE its own os.walk loop, so the mutation lands before os.walk
    descends.
    """
    for tree in roots or ROOTS:
        for dirpath, dirnames, filenames in os.walk(tree):
            yield tree, dirpath, dirnames, filenames


def walk_flat(roots=None):
    """`walk` without the tree column, for callers that key by repo-relative path."""
    for _tree, dirpath, dirnames, filenames in walk(roots):
        yield dirpath, dirnames, filenames


def rel_or_raise(path):
    """`module_rel`, but a path outside every tree is a bug in the caller, not a None to thread."""
    rel = module_rel(path)
    if rel is None:
        raise ValueError("not under a DLL source root: %s" % path)
    return rel
