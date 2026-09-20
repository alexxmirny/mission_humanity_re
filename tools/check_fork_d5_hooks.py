#!/usr/bin/env python3
"""check_fork_d5_hooks.py -- fork item F3C: D5's teeth.

THE CLAIM UNDER TEST (the fork plan, ruling D5): "Only mh.dll uses `install_trampoline`. Every
other module registers through a concrete hook API (named hook points ...). The entry-claim registry
stays mh.dll-private."

At F4 the determinism harness becomes `mh_harness.dll`. If it still called the raw inline-detour
primitives it would be a separate binary writing `E9` bytes into the game across a DLL boundary,
carrying its own copy of every target VA, steal width, entry-claim decision and arm-guard constant.
F3C moved all of that into the named hook-point table (`src/mh_dll/mh/hook/hookpoint.{h,cpp}`), and
this gate is what keeps it moved -- because the regression is a ONE-LINE regression: the next author
who needs a hook in harness.cpp reaches for `install_trampoline` exactly as fifteen sites did before,
the build is green, the log is unchanged, and the boundary is gone with nothing to notice it.

---- WHAT IS FORBIDDEN, AND WHY EACH ENTRY IS ON THE LIST ----------------------------------------

In HARNESS-OWNED code (the set below), zero occurrences of:

  install_trampoline / install_jmp / patch_bytes_guarded
      the three primitives that write over a game entry. The measured population: 77 call sites
      tree-wide before F3C, 14 of them the harness's.
  detour_refusal
      the adjudication in FRONT of a write. Not a write itself -- but a caller asking it is a caller
      that already holds the target, the claim and the guard bytes, which is the whole coupling. The
      hook API answers the same question as `available(point)`.
  VirtualProtect
      because otherwise "zero raw-primitive callers" is satisfiable by hand-rolling the write, and
      that is not hypothetical: `MH_Harness_LateArm`'s enqueue neuter did exactly that, with its own
      `VirtualProtect` + three byte stores and no primitive in sight, which is why it never appeared
      in the 77. `VirtualAlloc` is NOT forbidden -- the harness allocates its own buffers with it.

---- AND THE POSITIVE ARM, WHICH IS THE HALF THAT MAKES IT A GATE --------------------------------

A zero-survivor check passes just as green over a file that arms NOTHING as over one that arms
everything through the API (an instrument that can only report absence
cannot tell "nothing to find" from "not looking"). So the gate ALSO requires that harness-owned code
still arms through the hook API at least `MIN_ARMS` times, and that every point it names exists in
the enum. Delete the harness's hooks and this goes RED, not green.

Every assertion is re-fired in --selftest against planted violations in a temp tree, the house
pattern (check_fork_f2_drop, check_fork_d8).

NOT THIS GATE'S BUSINESS: the three R7 `mh::sim::` installs that left net seam code in the same
item. `check_net_lockstep_refs` already enumerates every net->sim/lockstep reference and fails
UNRULED (exit 2) on a re-introduced one, which is the louder failure of the two. One claim, one
tool.

Usage:
  python tools/check_fork_d5_hooks.py            # the gate
  python tools/check_fork_d5_hooks.py --selftest # planted-violation reds + walker liveness

Exit 0 = clean. Exit 1 = a violation (or the walker found nothing, which is never a pass).
"""

from __future__ import annotations

import os
import tempfile
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src", "mh_dll")

sys.path.insert(0, os.path.join(REPO, "tools"))
from check_net_lockstep_refs import strip_comments_and_strings  # noqa: E402

# ---------------------------------------------------------------------------------------------
# HARNESS-OWNED CODE. Enumerated, not globbed by keyword, so the set is reviewable -- and every
# named file must EXIST or the run refuses: a gate whose scope silently emptied is the failure it is
# supposed to catch. `harness*.cpp` covers a future split of the 8k-line file without an edit here.
#
# What is deliberately NOT in this set, and why: every other raw-primitive caller is mh.dll's own.
# seams/{net_seams,net_lockstep,net_diag}.cpp are the NET WIRING, which ruling D4 keeps in mh.dll
# calling mh_net.dll (mh_net.dll is transport only). seams/{video,ui_pause,ui_keyrepeat,mp_menu,
# launch,standalone,reimpl_probe}.cpp are the UI/lobby fixups and the patch host, D4's module inside
# mh.dll. save/save_live.cpp is the save walker's own entry guard. D5 forbids the primitives to
# EVERY OTHER MODULE, and after F3C the harness is the only other module there is.
# ---------------------------------------------------------------------------------------------
HARNESS_OWNED = [
    os.path.join("mh_harness", "harness.cpp"),
]
# F4E MADE THE SPLIT REAL, and the set follows the MODULE rather than the directory. Fork F5O
# then made directory and module AGREE: harness.cpp moved to mh_harness\ beside the instrument's
# other two TUs, so one glob covers the whole set and the mh\seams\ glob F4E needed is gone.
# What that second glob DELIBERATELY EXCLUDED, and what the surviving one still must not reach,
# is mh\seams\harness_bind.cpp: that is MH.DLL's side of the
# boundary: the LoadLibrary, the twelve forwarding shims and the configured-but-absent refusal,
# and it is no more harness-owned than module_bind.cpp is mh_net-owned. Ruling D5 forbids the raw
# primitives to every module OTHER than mh.dll, so putting mh.dll's own bind file in this set would
# be scoping the gate by filename rather than by ownership.
HARNESS_OWNED_GLOBS = [
    (os.path.join("mh_harness"), re.compile(r"^.*\.cpp$")),
]

FORBIDDEN = {
    "install_trampoline": "the trampoline primitive (use mh::hook::arm_observer)",
    "install_jmp": "the whole-body primitive (use mh::hook::arm_replacement)",
    "patch_bytes_guarded": "the byte-patch primitive (add a point, or keep the patch in mh.dll)",
    "detour_refusal": "the raw adjudication (use mh::hook::available)",
    "VirtualProtect": "a hand-rolled entry write (use mh::hook::arm_neuter)",
}
CALL_RE = {name: re.compile(r"\b%s\s*\(" % re.escape(name)) for name in FORBIDDEN}

# The API the harness must be seen using instead. `arm_*` and `available`/`entry_bytes_match` are the
# arming surface; `register_callback` is the observer surface.
ARM_RE = re.compile(
    r"mh::hook::(arm_observer|arm_replacement|arm_neuter|available|entry_bytes_match|register_callback)\s*\("
)
POINT_RE = re.compile(r"mh::hook::point::([A-Za-z_]\w*)")

# Measured at F3C: 14 install sites + 1 hand-rolled neuter + 2 joint pre-checks + 4 entry-byte
# guards + 2 observer registrations. Set as a FLOOR well under that, so removing a facility on
# purpose does not red the gate while deleting the boundary does.
MIN_ARMS = 12

HOOKPOINT_H = os.path.join(SRC, "mh", "hook", "hookpoint.h")


def enum_points(hookpoint_h=None):
    """The `point` enum's members, read out of the header. Returns None if it cannot be parsed --
    which is a REFUSAL upstream, not an empty set: an unparseable header would otherwise make every
    point name the harness uses look invalid, or (worse, with the test inverted) make none of them
    checkable at all."""
    path = hookpoint_h or HOOKPOINT_H
    if not os.path.exists(path):
        return None
    code = strip_comments_and_strings(open(path, encoding="utf-8", errors="replace").read())
    m = re.search(r"enum\s+class\s+point\s*\{(.*?)\}\s*;", code, re.S)
    if not m:
        return None
    return {t for t in re.findall(r"\b([A-Za-z_]\w*)\b", m.group(1))} or None


def owned_files(root):
    """The harness-owned sources under `root`, as (relative path, absolute path)."""
    out = []
    for rel in HARNESS_OWNED:
        out.append((rel, os.path.join(root, rel)))
    for subdir, pat in HARNESS_OWNED_GLOBS:
        d = os.path.join(root, subdir)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if pat.match(name):
                rel = os.path.join(subdir, name)
                if rel not in HARNESS_OWNED:
                    out.append((rel, os.path.join(d, name)))
    return out


def check(root=SRC, hookpoint_h=None, say=print, require_points=True):
    fails = []
    files = owned_files(root)
    if not files:
        say(
            "[FAIL] no harness-owned source found under %s -- the scan found NOTHING to check"
            % root
        )
        return 1

    arms = 0
    scanned = 0
    points_used = set()
    for rel, path in files:
        if not os.path.exists(path):
            fails.append("harness-owned source is MISSING: %s" % rel)
            continue
        scanned += 1
        raw = open(path, encoding="utf-8", errors="replace").read()
        code = strip_comments_and_strings(raw)
        for name, why in sorted(FORBIDDEN.items()):
            for m in CALL_RE[name].finditer(code):
                line = code[: m.start()].count("\n") + 1
                fails.append("%s:%d calls %s -- %s" % (rel, line, name, why))
        arms += len(ARM_RE.findall(code))
        points_used |= set(POINT_RE.findall(code))

    if arms < MIN_ARMS:
        fails.append(
            "harness-owned code arms through the hook API only %d time(s) (floor %d). A zero-survivor "
            "scan over code that arms NOTHING is not evidence -- see the note on the positive arm."
            % (arms, MIN_ARMS)
        )

    if require_points:
        known = enum_points(hookpoint_h)
        if known is None:
            fails.append(
                "cannot parse the `point` enum out of %s -- the run is UNSOUND, not clean"
                % (hookpoint_h or HOOKPOINT_H)
            )
        else:
            for p in sorted(points_used - known):
                fails.append(
                    "harness-owned code names mh::hook::point::%s, which is not in the enum" % p
                )

    for f in fails:
        say("[FAIL] %s" % f)
    if not fails:
        say(
            "check_fork_d5_hooks: PASS -- %d harness-owned file(s), zero raw-primitive callers, "
            "%d arm(s) through the named hook points (%d distinct)"
            % (scanned, arms, len(points_used))
        )
    return 1 if fails else 0


def _selftest_root(prefix):
    """One root for every temp tree the selftest makes, removed at interpreter exit.

    The selftest used to mkdtemp per synthetic tree and never remove any of them: measured
    2026-09-18 at 21,462 leaked f2drop_* dirs in %TEMP% (with d8_*, d5hooks_*, inmem_selftest_*
    and narration_selftest_* alongside) -- one lint run leaks a few, and the lint runs every
    session. mkdtemp(dir=root) keeps every tree under one directory that atexit removes.
    """
    import atexit
    import shutil

    root = tempfile.mkdtemp(prefix=prefix)
    atexit.register(shutil.rmtree, root, ignore_errors=True)
    return root


def selftest():
    import shutil
    import tempfile

    ok = True

    def expect(name, cond):
        nonlocal ok
        print("  [%s] %s" % ("ok" if cond else "FAIL", name))
        ok = ok and cond

    quiet = lambda *_: None  # noqa: E731

    sink = []
    rc = check(say=sink.append)
    expect("the real tree passes", rc == 0)
    if rc:
        for line in sink:
            print("    %s" % line)

    real = owned_files(SRC)
    expect("the walker actually finds the harness sources", len(real) >= 1)
    expect(
        "the point enum parses", (enum_points() or set()) and "sim_step" in (enum_points() or set())
    )

    root = _selftest_root("d5hooks_selftest_")

    def synth(extra="", drop_arms=False, no_header=False):
        """A temp tree holding a copy of the real harness source, optionally mutated."""
        d = tempfile.mkdtemp(prefix="d5hooks_", dir=root)
        dst = os.path.join(d, "mh_harness")
        os.makedirs(dst)
        body = open(os.path.join(SRC, HARNESS_OWNED[0]), encoding="utf-8", errors="replace").read()
        if drop_arms:
            body = ARM_RE.sub("noop(", body)
        body += "\n" + extra
        with open(os.path.join(dst, "harness.cpp"), "w", encoding="utf-8") as fh:
            fh.write(body)
        hp = os.path.join(d, "mh", "hook", "hookpoint.h")
        if not no_header:
            os.makedirs(os.path.dirname(hp))
            shutil.copyfile(HOOKPOINT_H, hp)
        return d, hp

    d, hp = synth()
    expect("an unmutated copy still passes", check(d, hp, say=quiet) == 0)

    for name in sorted(FORBIDDEN):
        d, hp = synth(extra="void rogue() { %s(1, 2); }\n" % name)
        expect("planted %s call -> red" % name, check(d, hp, say=quiet) == 1)

    d, hp = synth(extra="// a comment naming install_trampoline(x) is not a call\n")
    expect("a COMMENT naming a primitive is not a violation", check(d, hp, say=quiet) == 0)

    d, hp = synth(extra='const char *s = "install_jmp(";\n')
    expect("a STRING naming a primitive is not a violation", check(d, hp, say=quiet) == 0)

    d, hp = synth(drop_arms=True)
    expect(
        "stripping every hook-API arm -> red (the positive arm, G106)", check(d, hp, say=quiet) == 1
    )

    d, hp = synth(
        extra="void rogue() { mh::hook::arm_observer(mh::hook::point::not_a_point, 0, 0); }\n"
    )
    expect("a point name that is not in the enum -> red", check(d, hp, say=quiet) == 1)

    d, hp = synth(no_header=True)
    expect("an unreadable point enum is a REFUSAL, not a pass", check(d, hp, say=quiet) == 1)

    expect(
        "an empty tree is REFUSED, not passed",
        check(tempfile.mkdtemp(prefix="d5hooks_empty_", dir=root), HOOKPOINT_H, say=quiet) == 1,
    )

    print("check_fork_d5_hooks --selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    return check()


if __name__ == "__main__":
    sys.exit(main())
