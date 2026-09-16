"""Mutation-test engine for the reimplementation modules: break a translation N ways, prove each
break is CAUGHT by an assertion that names it, and leave the tree exactly as it was found.

A green check that cannot go red is worth less than no check -- that is the whole argument for
mutation-testing these translations, and it matters most for the properties no rig run can separate
(inclusive loop bounds, signed-vs-unsigned compares, a MOVZX on a field that is never large in
practice). Those are the cases where the offline oracle is the only witness there will ever be.

This is the ENGINE. A campaign supplies its own spec and calls `main`; the driver lives here so the
three ways the 2026-08-02 sweep damaged the working tree cannot be re-introduced per campaign:

1. THE ORIGINAL IS KEPT ON DISK, not only in memory. The previous driver held the pristine text in a
   local variable, so killing it mid-iteration left the mutated file on disk -- and the NEXT run then
   read that corrupted text as its "original" and mutated it again. Two source files needed hand
   repair. Here every touched file gets a `.mutorig` sibling before the first edit; the restore is in
   a `finally`, and a leftover `.mutorig` found at startup is treated as an interrupted run and
   restored before anything else happens.
2. EVERY SUBPROCESS HAS A TIMEOUT, and a timeout kills the process TREE (`tools/proc.py`). A mutation
   that swallows a loop's advance produces an infinite loop in the test binary; without a timeout
   that is an orphaned process pinning a core, which is what happened.
3. A WHITESPACE-TOLERANT ANCHOR KEEPS ITS LEADING WHITESPACE LITERAL. Anchors are transcribed from
   the source, and `clang-format` re-aligns trailing comments whenever a nearby line changes width,
   so a literal anchor silently degrades into "ANCHOR NOT FOUND" -- which reports as a MISSED
   mutation and reads as a weak assertion. But a pattern that OPENS with `\\s+` starts matching at
   the newline before the anchor and eats it, splicing the mutated statement onto the tail of the
   preceding comment. Only INTERNAL whitespace runs are relaxed. See `anchor_pattern`.

Usage from a campaign script:

    from mutate import Mutation, main
    MUTATIONS = [Mutation(SRC, "what this breaks", old_text, new_text), ...]
    sys.exit(main(MUTATIONS, mode="aitest"))

CLI: `python tools/oneoff/<campaign>.py [lo [hi]]` runs the `[lo, hi)` slice, so a long sweep fits
inside a foreground call's timeout in halves rather than needing a background launch.
"""

from __future__ import annotations

import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import proc  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
BAT = REPO / "src" / "mh_dll" / "mh_nettest" / "build_selftest.bat"
EXE = Path.home() / "AppData/Local/Temp/mh_nettest/net_selftest.exe"

BUILD_TIMEOUT_S = 900
# Deliberately tight. The suite runs in seconds; anything approaching this is a mutation that turned
# a loop infinite, and the point is to kill it and report rather than to wait it out.
TEST_TIMEOUT_S = 180
BACKUP_SUFFIX = ".mutorig"


@dataclass(frozen=True)
class Mutation:
    src: Path
    name: str
    old: str
    new: str


def anchor_pattern(old: str) -> re.Pattern:
    """Match the anchor with INTERNAL whitespace relaxed and LEADING whitespace literal.

    See this module's docstring, point 3: relaxing the leading run is what corrupted the tree.
    """
    lead = re.match(r"^\s*", old).group(0)
    parts = re.split(r"(\s+)", old[len(lead) :])
    body = "".join(r"\s+" if p and not p.strip() else re.escape(p) for p in parts)
    return re.compile(re.escape(lead) + body)


def _backup(path: Path) -> Path:
    return path.with_suffix(path.suffix + BACKUP_SUFFIX)


def restore_interrupted(paths) -> list[Path]:
    """Undo a previous run that was killed mid-iteration. Call before reading any 'original'."""
    restored = []
    for path in paths:
        bak = _backup(path)
        if bak.exists():
            path.write_text(bak.read_text(encoding="utf-8"), encoding="utf-8", newline="")
            bak.unlink()
            restored.append(path)
    return restored


def build_and_run(mode: str):
    """Build the selftest and run one mode. Returns (stdout, error_text) -- exactly one is None."""
    try:
        b = proc.run_capture(["cmd", "/c", str(BAT)], timeout=BUILD_TIMEOUT_S, cwd=REPO)
    except subprocess.TimeoutExpired:
        return None, f"BUILD TIMED OUT after {BUILD_TIMEOUT_S}s (process tree killed)"
    if b.returncode != 0 or not EXE.exists():
        return None, (b.stdout or "")[-1500:] + (b.stderr or "")[-1500:]
    # RUN IT TWICE IF THE FIRST COMES BACK EMPTY. Measured 2026-08-03: on a long sweep the exe
    # occasionally returns with NO stdout at all -- the build has just rewritten it, and the launch
    # loses the stream (an on-access scanner is the likeliest culprit; the same shape produced stray
    # `exit 116` runs by hand the same evening). An empty capture is indistinguishable from a
    # mutation the assertions missed, and on that sweep it mislabelled four of sixteen. One retry
    # settles it, and a mutation that really produces no output will simply report twice.
    for _ in range(2):
        try:
            r = proc.run_capture([str(EXE), mode], timeout=TEST_TIMEOUT_S, cwd=REPO)
        except subprocess.TimeoutExpired:
            # Not a failure of the harness: a mutation that hangs the binary IS caught, loudly.
            return None, f"TEST HUNG >{TEST_TIMEOUT_S}s -- mutation removed a loop's advance"
        if (r.stdout or "").strip():
            return r.stdout, None
    return r.stdout, None


def main(mutations, *, mode="aitest", argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    lo = int(argv[0]) if len(argv) > 0 else 0
    hi = int(argv[1]) if len(argv) > 1 else len(mutations)

    paths = sorted({m.src for m in mutations})
    for p in restore_interrupted(paths):
        print(f"!! restored {p} from an interrupted previous run")

    originals = {p: p.read_text(encoding="utf-8") for p in paths}
    for p, text in originals.items():
        _backup(p).write_text(text, encoding="utf-8", newline="")

    results = []
    try:
        for m in mutations[lo:hi]:
            original = originals[m.src]
            pat = anchor_pattern(m.old)
            if not pat.search(original):
                results.append((m.name, "ANCHOR NOT FOUND", []))
                print("---", m.name, "-> ANCHOR NOT FOUND", flush=True)
                continue
            m.src.write_text(
                pat.sub(lambda _mo: m.new, original, count=1), encoding="utf-8", newline=""
            )
            try:
                out, err = build_and_run(mode)
            finally:
                m.src.write_text(original, encoding="utf-8", newline="")
            if out is None:
                # A hang or a build failure both mean the mutation could not produce a green
                # binary, which is a CAUGHT mutation -- but say which, because a build failure can
                # also mean the anchor produced nonsense.
                results.append((m.name, err.splitlines()[0][:80], ["(did not build/run)"]))
                print("---", m.name, "->", results[-1][1], flush=True)
                continue
            fails = [ln.strip() for ln in out.splitlines() if "FAIL" in ln]
            tail = [ln for ln in out.splitlines() if "checks," in ln]
            if not tail:
                # The binary ran but never printed its summary -- it died part-way, or the output was
                # truncated. Either way the verdict is UNKNOWN, not MISSED, and the bare "?" this
                # used to print read as the latter: the 2026-08-03 campaign reported two mutations as
                # MISSED that a by-hand run then showed were caught. Say so, and show what did come
                # back, so the next reader re-runs that one instead of weakening a good assertion.
                head = " | ".join(ln.strip() for ln in out.splitlines()[:3] if ln.strip())
                results.append((m.name, "NO SUMMARY LINE -- rerun by hand: " + head[:120], fails))
            else:
                results.append((m.name, tail[-1], fails))
            print("---", m.name, "->", results[-1][1], flush=True)
            for f in fails[:4]:
                print("    ", f)
    finally:
        for p, text in originals.items():
            p.write_text(text, encoding="utf-8", newline="")
            _backup(p).unlink(missing_ok=True)

    print("\n==== SUMMARY ====")
    ok = True
    for name, tail, fails in results:
        # Three verdicts, not two. A run that produced no summary line has told us nothing about the
        # assertion, so calling it MISSED would send the reader off to strengthen a check that is
        # probably fine -- which is exactly what happened on 2026-08-03.
        if tail.startswith("NO SUMMARY LINE") and not fails:
            ok = False
            print("UNKNOWN " + name + "  [" + tail + "]")
            continue
        good = bool(fails)
        ok = ok and good
        print(("CAUGHT  " if good else "MISSED  ") + name + "  [" + tail + "]")

    out, err = build_and_run(mode)  # prove the tree is back to green
    # A build that produced NO output at all used to crash here with IndexError -- after the restore,
    # so the tree was fine, but the run's exit status became the traceback's rather than the
    # campaign's and a MISSED mutation could be mistaken for a tooling failure. An empty result is a
    # real answer ("the revert build said nothing"), not an impossible one.
    lines = (out or err or "").strip().splitlines()
    print(
        "\nrevert check:",
        lines[-1]
        if lines
        else "(no output from the revert build -- rerun it by hand before trusting this campaign)",
    )
    return 0 if ok else 1
