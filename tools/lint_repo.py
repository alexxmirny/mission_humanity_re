#!/usr/bin/env python3
"""Repo-side lint: the fast, Ghidra-free checks that guard the src/mh_dll + tools trees.

EVERY CHECK ADDED HERE MUST BE HERMETIC. This is not a style preference. An unattended `migration_loop`
session runs `lint_repo` as part of its gate, so anything a check writes, it writes INSIDE a live
loop -- against the same repo, the same driver state files, the same processes. On 2026-08-03 the
`migration_loop selftest` check wrote the real `session_reports/loop/stop.request` in order to test the
stop-file watcher; the live loop's watcher (a different process, polling every second) read it and
wound the run down. It killed two real sessions from inside their own gate and presented as the
operator mis-pressing Ctrl+C. A check that needs to exercise state must redirect that state to a
temp dir AND assert afterwards that the real paths were untouched -- the damage is invisible when
nothing is running, which is exactly when you are testing it.

Runs four checks (each independently reported; exit code 1 if ANY fails):
  0. dump-coherence -- tools/lint_dump_coherence.py: the DB dumps agree with each other. Checks 1-3
                    below all compare a GENERATED header against a dump, so a STALE DUMP is
                    invisible to them; this is the gate that sees it (added 2026-08-31).
  1. addr-drift  -- tools/gen_dll_addrs.py --check: the generated mh_addrs.gen.h still matches the
                    manifest + EN symbol dump (a Ghidra rename/move surfaces as a diff, not a stale VA).
  2. struct-drift -- tools/gen_dll_structs.py --check: the generated mh_structs.gen.h still matches
                    dll_struct_layouts.json (a Ghidra struct retype/reorder surfaces as a diff).
  3. calls-drift  -- tools/gen_dll_calls.py --check: the generated mh_calls.gen.{h,cpp} + the thunk
                    oracle still match dll_call_protos.json (a Ghidra prototype change -- a new
                    committed convention, a corrected return type -- surfaces as a diff, not as a
                    wrong register contract at a call site).
  3b. region-mirror -- the determinism manifest is encoded twice (the DLL's generated HASH_REGIONS[]
                    and mp_analyze's REGION_NAMES + STATE_EXCLUDED); a positional mismatch silently
                    re-labels every desync report. Both ends are generated from
                    tools/data/hash_manifest.json since ST2M, so this is now a check on the
                    ARTEFACTS rather than on two hand-kept lists. tools/lint_region_mirror.py.
  3c. patch/seam interlock -- a DLL byte patch aimed inside a function the tree PROMOTES writes bytes
                    that will never execute, while patch_bytes_guarded still reports "armed". The
                    generated promotable-extent header must be current (gen_dll_patches --check), every
                    patch call site must be registered in dll_patch_manifest.json with an owner that
                    still resolves, and any patch owned by a promotable function must declare how the
                    fix survives promotion. Its --selftest re-asserts that all eight checks still go RED
                    on the defect each exists to catch -- the bug being prevented is silence, so a check
                    that quietly stopped firing is the same failure. tools/lint_dll_patches.py.
  3d. save-table-drift -- the save format's block table is EXTRACTED from the disassembly
                    (tools/data/save_block_table.json) and turned into C++ by
                    gen_save_table_header.py, which pairs the writer's sequence against the reader's
                    as it generates. --check re-runs that generation and fails if the committed
                    save_table.gen.h differs OR if the two directions stop agreeing -- so an edited
                    table, or a hand-edited header, cannot silently re-frame a save file. (The
                    re-derivation FROM THE BINARY needs the Ghidra-exported .asm listings and stays a
                    tool run; what lint owns is the JSON -> header step and the pair check.)
  4. clang-format -- --dry-run -Werror over the LIVE src/mh_dll sources (the tree is fully formatted
                    and idempotent since 2026-07-23, so any hit is real drift). Respects
                    .clang-format-ignore (generated addr/save headers, attic) --
                    the same exclusions are applied here explicitly so the check does not depend on
                    the clang-format version honoring the ignore file.
  4b. doc-symbols -- ADVISORY (does not fail the gate). Every llm_/_G_LLM_ name cited in docs/ still
                    exists in the generated EN symbol index. A rename silently orphans the prose that
                    explains the mechanism, and the docs are the only place that knowledge lives. It
                    is advisory because bare-NAME scanning mis-reads the docs' compact set notation;
                    the checkable form is address-keyed (tools/resolve_doc_refs.py).
                    tools/lint_doc_symbols.py.
  5. ruff        -- ruff format --check tools (config: ruff.toml).
  6. rust        -- tools/lint_rust.py: cargo fmt --all --check + cargo clippy --workspace
                    --all-targets -- -D warnings, over the src/launcher + src/relay workspace. The
                    ONLY rows in this file with a three-valued verdict: cargo is a per-machine
                    toolchain (rustup), so on a machine without it they report SKIP with the remedy
                    rather than passing vacuously. See rust_row().

This is the per-session repo lint (refactor Phase 5); the Ghidra-side
annotation lint stays separate (tools/lint_annotations.py, needs a live ReVA session).

Usage: python tools/lint_repo.py [--fix]   (--fix applies clang-format/ruff instead of checking)
       python tools/lint_repo.py --ci      (the PUBLIC-tree subset -- see CI_SKIPS below)

--ci (fork F5D) is what makes this file runnable on a clone that has no private research layers, no
Ghidra export directory and no retail game copy. It does NOT weaken a single check: it SKIPS a
declared list of rows, BY NAME, in the run output, and prints total/run/skipped. Every other row
runs exactly as it does here. See CI_SKIPS for the list and the measured reason behind each entry.

WHAT --ci DOES NOT MEAN (measured at fork F5M): it does not mean "this is the public tree". The
workflow that passes it is dispatched by hand on this repo's private GitHub remote as well, so --ci
runs against the PRIVATE tree at least as often as against the cut. A row that must behave
differently on the two trees therefore DETECTS the tree -- see _tree_is_the_cut(), which asks the
publish ledger's declared `_public_tree_marker` -- and never keys off this flag.
"""

import argparse
import contextlib
import io
import os
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import machine_config as machine  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLANG_FORMAT = os.environ.get("MH_CLANG_FORMAT", machine.CLANG_FORMAT)
# "libmh" joined at fork F5O, when the 628-TU roster moved out of mh/ into the directory of
# the project that owns it. Leaving it out would have dropped 1260 files out of clang-format
# silently -- the scope-emptying failure this list is written as a literal to make visible.
DLL_ROOTS = ("mh", "libmh", "mh_common", "mh_nettest", "libmh_test")
# The proxy-DLL shims are a separate tree but the same C++ discipline applies,
# so they are formatted with the rest. Their generated stub bodies are excluded below: 47 naked
# inline-asm thunks are emitted by tools/gen_proxy_stubs.py and clang-format would only fight it.
SHIM_ROOT = os.path.join("src", "mh_shim")
DLL_EXCLUDE = (
    "attic",
    "proxy_stubs.gen.cpp",
    os.path.join("addr", "mh_addrs.gen.h"),
    os.path.join("addr", "mh_structs.gen.h"),
    os.path.join("addr", "mh_calls.gen.h"),
    os.path.join("addr", "mh_calls.gen.cpp"),
    os.path.join("addr", "mh_export.gen.h"),
    os.path.join("addr", "mh_patches.gen.h"),
    os.path.join("addr", "mh_regions.gen.h"),
    os.path.join("save", "save_table.gen.h"),
    "mh_calls_selftest.gen.cpp",
    "mh_export_selftest.gen.cpp",
    os.path.join("addr", "mh_hostapi_bind.gen.cpp"),
    "mh_hostapi_selftest.gen.cpp",
    # The rebind binder (tools/gen_libmh_rebind.py). Excluded for the same reason as its siblings:
    # 626 one-line #defines and 626 static_asserts that clang-format would only re-align. The
    # HAND-written state/rebind_shims.{h,cpp} are NOT excluded -- they are ordinary source.
    os.path.join("addr", "mh_rebind.gen.h"),
    os.path.join("state", "rebind_targets.gen.h"),
    os.path.join("state", "rebind_verify.gen.cpp"),
    os.path.join("state", "rebind_traps.gen.cpp"),
)


# ------------------------------------------------------------------------------------------------
# fork F5D -- THE --ci SKIP LIST.
#
# DECLARED DATA, deliberately: one table, keyed by the EXACT row name, with the measured reason next
# to it. The alternative -- an `if args.ci: return` scattered through forty call sites -- is the
# shape where a row quietly stops running and nobody can enumerate what the public gate does not
# cover. Here the list can be read, counted, and checked against the roster, which is what
# `--ci-selftest` does on every lint run (see check_ci_skip_list): a key naming a row that no longer
# exists FAILS, so a skip cannot outlive the check it excuses. That arm runs in BOTH modes, so the
# private tree's own lint is what catches a stale entry.
#
# MEASURED, not assumed, on a materialized publish-set tree with every withheld path physically
# absent, no game, no rig, no tmp/ (2026-09-15 at F5D, re-measured 2026-09-16 at F5M after the Q4
# and Q2/Q3/Q5 flips moved the line twice). The live counts are the runner's own total/run/skipped
# line and this table's own length, never a number written here -- the F5D figure was stale within
# a day of the Q4 flip, which is how the class (d) rows below went unnoticed, and it moved again
# at F5M S4b when the archive-tool cut unwired nine of this table's rows. FOUR classes, and
# only the first and last are about publishability at all:
#
#   (a) PRIVATE RESEARCH LAYERS -- now EMPTY. It held seven entries until fork F5M S4b, when the
#       archive-tool cut removed the ROWS they excused rather than excusing them again. The
#       re-measured correction F5D recorded here is worth keeping even though its subjects are
#       gone: several rows F5A predicted would fail on the public tree PASSED (hermetic selftests),
#       and one of them passed VACUOUSLY -- it found zero of the assets it parses and returned 0.
#       A vacuous pass is not a gate, and that is a reason to unwire a row, not to keep it.
#   (b) A GITIGNORED GHIDRA EXPORT (2 rows) -- tmp/decomp_orders_issue/*.asm and
#       tmp/state_matrix.json. NOT a publishability question: these rows fail on ANY clean clone,
#       this repo's included, because their input is produced by a Ghidra run and never committed.
#       Proven by causation, not inference: dropping the two tmp/ artifacts into the public tree
#       turns both rows green with nothing else changed.
#   (c) A RETAIL GAME COPY (1 row) + THE LEDGER'S OWN SUBJECT (2 rows). check_inmem_patch_parity
#       --selftest patches machine.POLYGON_CLEAN's mh.exe. check_publishable dispositions the
#       PRIVATE tree: on the public tree the five withhold rules match nothing, which is its
#       dead-rule failure by construction. It is the cut's gate, not the public tree's.
#   (d) A WITHHELD COMPARISON DOCUMENT (2 rows, fork F5M) -- a drift gate whose right-hand side is
#       a doc under docs/ that the Q4 answer withheld. Not predicted by anyone: these rows were
#       GREEN in F5D's rehearsal and went red at F5E, and the public gate was not re-measured in
#       between. The entries carry the measured cost of each skip, which is the part a class label
#       hides.
#
# A row that PASSES on the public tree is NOT in this table, however privately-flavoured it looks.
CI_SKIPS = {
    # --- (a) private research layers: EMPTY since fork F5M S4b -------------------------------
    # Seven entries stood here -- the session-log index and its selftest, the tracker<->ledger
    # agreement, the loop's graceful stop, the state-interface wiring pair, and the advisory
    # doc-symbol scan. Every one of them excused a row whose TOOL is class=archive, and the archive
    # cut withheld those tools, so the rows themselves are gone and an entry excusing a row that no
    # longer exists is exactly what --ci-selftest refuses. Class (a) is kept as a heading rather
    # than deleted because the distinction it names is still the one a future skip has to justify:
    # is this row skipped because the PUBLIC TREE lacks the file, or because the RUNNER lacks a
    # tool? Only the first kind belongs in a cut, and the right answer for it is usually to unwire
    # the row rather than to skip it -- which is what S4b did to all seven.
    # --- (b) a gitignored Ghidra export ------------------------------------------------------
    "order-issue golden drift (gen_order_issue_golden --check)": (
        "tmp/decomp_orders_issue/*.asm -- Ghidra-exported listings, gitignored, so this row cannot "
        "run on ANY clean clone (not a publishability question)"
    ),
    "state-registry drift + save-block coverage + ownership interlock (gen_state_registry --check)": (
        "tmp/state_matrix.json -- a gitignored Ghidra-derived enrichment whose ABSENCE changes "
        "merge()'s output, so this row cannot run on ANY clean clone either"
    ),
    # --- (c) a retail game copy, and the ledger's own subject --------------------------------
    "inmem parity oracle, off-rig (check_inmem_patch_parity --selftest)": (
        "a retail game copy (machine.POLYGON_CLEAN/mh.exe) -- bring-your-own-game"
    ),
    "fork F5E citations: no published file cites a path the cut does not carry (--strict)": (
        "the PRIVATE tree it scans. On the public tree every REMAINING private citation resolves "
        "to nothing and the DANGLING arm fires on all of them: this gates the CUT, not the cut's "
        "output. The publish-ledger rows used to sit here for the same reason and no longer do -- "
        "they detect the tree and run the mirror arm instead (fork F5M); this one cannot follow "
        "them until its Direction-B residue is gone. F5M MEASURED that the residue does NOT go to "
        "zero on the Q2/Q3/Q5 answers: 271 -> 134 lines over 52 files, and S4b's archive cut took "
        "it to 60 over 31 by re-authoring every citation of a now-withheld tool. What is left "
        "cites targets that are withheld for good (the failure ledger, the trackers, the agent "
        "workflows). It needs a drain or a ruled frozen baseline, not an answer. The --selftest row is NOT "
        "skipped: it builds its own synthetic tree, and its one arm that reads the REAL tree was "
        "measured to draw 11 hits from 5 files, every one of them in the publish set, against a "
        "threshold of 5 -- so it survives the cut."
    ),
    # --- (d) a withheld comparison document: RESOLVED, not skipped (fork F5M S4b) ----------
    # Two entries stood here, for the two order-generator drift rows whose right-hand side is a
    # research document the Q4 answer withheld. They were a REGRESSION OF THAT FLIP rather than of
    # F5M -- the public gate was last rehearsed green BEFORE it, and nothing re-ran it afterwards --
    # and S2 declared them here so the gate could go green while the shape was still open. S4b
    # settled it the honest way: both generators are class=archive, the cut withholds them, and a
    # drift gate whose comparison document the tree does not carry is not a gate that should be
    # skipped, it is a row that should not be there. The measured cost is recorded with the rows
    # (search the tombstones for ORDER-ISSUE and ORDER-MATRIX), and the one loss worth naming is
    # the order-matrix JSON's own drift check -- the JSON publishes, but it is re-derived only from
    # Ghidra dumps a private session regenerates, so the public tree could not act on the answer.
}

CI_MODE = False
_DECLARED = []  # every row name declared this run, in declaration order
_SKIPPED = []  # (name, reason) for the rows --ci held back

_TREE_IS_CUT = None


def _tree_is_the_cut():
    """Is this checkout the PUBLIC CUT rather than the private working tree? (fork F5M)

    Asked of the publish ledger, which declares the answer in its `_public_tree_marker` block --
    NOT inferred from --ci. --ci is a statement about what is INSTALLED on the runner (no game, no
    rig, no Ghidra export); it is not a statement about which tree was checked out, and .github's
    workflow is dispatched on the private remote as well. Cached: the answer costs a
    `git ls-files` and declare_checks runs three times under --ci-selftest.

    Falls back to `private` if the ledger machinery is unreadable, which keeps the rows on the arm
    that has always run rather than switching them silently."""
    global _TREE_IS_CUT
    if _TREE_IS_CUT is None:
        try:
            import check_publishable as cp  # noqa: PLC0415

            led = cp.load_ledger()
            _TREE_IS_CUT = cp.is_public_tree(cp.tracked_paths(REPO), led)
        except Exception:
            _TREE_IS_CUT = False
    return _TREE_IS_CUT


def cxx_files():
    base = os.path.join(REPO, "src", "mh_dll")
    trees = [os.path.join(base, root_name) for root_name in DLL_ROOTS]
    trees.append(os.path.join(REPO, SHIM_ROOT))
    for tree in trees:
        for dirpath, _dirs, files in os.walk(tree):
            for fn in files:
                if not fn.endswith((".cpp", ".h", ".c")):
                    continue
                p = os.path.join(dirpath, fn)
                if any(x in p for x in DLL_EXCLUDE):
                    continue
                yield p


# ------------------------------------------------------------------------------------------------
# The checks are QUEUED, then executed across a thread pool, then REPORTED IN DECLARATION ORDER.
#
# Measured 2026-08-23: 45 checks, 57 s wall, ONE subprocess at a time on an 8-core box -- and the
# slowest of them (check_const_view 19.5 s) shares nothing with anything else. The roster is
# four checks shorter since F2D retired the shadow oracle; the wall figure predates that.
# Every check here is already required to be HERMETIC (see the module docstring), which is exactly
# the property that makes running them concurrently legal rather than merely faster.
#
# EXCEPT for the one thing hermetic does not cover: a tool and its own `--selftest` are the SAME
# script, and several of those selftests demonstrate their check by staging a mutated copy of the
# state the plain run reads. So the lane key is the SCRIPT, not the check -- two invocations of
# gen_state_registry.py stay ordered with respect to each other, while unrelated scripts overlap
# freely. That buys the concurrency without asking each tool to prove it is thread-safe.
#
# `--fix` FORCES --jobs 1. In that mode ruff and clang-format REWRITE the same trees the read-only
# checks are parsing, which is a genuine race rather than a hermeticity question, and --fix is not
# the mode anybody is waiting on.
#
# Output order is INDEPENDENT of completion order on purpose: a gate log that reshuffles itself from
# run to run cannot be diffed against the last one.
_QUEUE = []


def _lane_of(cmd):
    """The serialization lane for a command -- the SCRIPT it drives, so a tool's `--check` and its
    `--selftest` never run at the same time."""
    if len(cmd) > 1 and os.path.basename(cmd[0]).lower().startswith("python"):
        return os.path.basename(cmd[1]).lower()
    return os.path.basename(cmd[0]).lower()


class _Check:
    __slots__ = ("name", "cmd", "cwd", "advisory", "lane", "fn", "done", "ok", "text")

    def __init__(self, name, cmd=None, cwd=REPO, advisory=False, fn=None, lane=None):
        self.name, self.cmd, self.cwd, self.advisory, self.fn = name, cmd, cwd, advisory, fn
        self.lane = lane or (_lane_of(cmd) if cmd else "fn:" + name)
        self.done = threading.Event()
        self.ok, self.text = True, ""


def _declare(name):
    """Record a row on the roster and answer whether --ci holds it back.

    EVERY row passes through here whether or not it is skipped -- the roster is what makes the
    printed total/run/skipped honest and what lets the dead-skip arm see a name that no longer
    exists."""
    _DECLARED.append(name)
    if CI_MODE and name in CI_SKIPS:
        _SKIPPED.append((name, CI_SKIPS[name]))
        return True
    return False


def check(name, cmd, cwd=REPO, advisory=False):
    """Queue a subprocess check. An advisory check reports but never fails the gate."""
    if _declare(name):
        return
    _QUEUE.append(_Check(name, cmd=cmd, cwd=cwd, advisory=advisory))


def check_fn(name, fn, advisory=False, lane=None):
    """Queue an in-process check: `fn` prints its own lines and returns its verdict (None == ok)."""
    if _declare(name):
        return
    _QUEUE.append(_Check(name, fn=fn, advisory=advisory, lane=lane))


def check_chunked(name, cmd_prefix, files, cwd=REPO):
    """Queue the chunked formatter check (see run_chunked)."""
    check_fn(
        name,
        lambda: run_chunked(name, cmd_prefix, files, cwd),
        lane=os.path.basename(cmd_prefix[0]).lower(),
    )


class _ThreadStdout:
    """sys.stdout split BY THREAD: a lane thread that registered a buffer writes there, everyone else
    writes to the real stream.

    contextlib.redirect_stdout swaps the PROCESS-WIDE sys.stdout, so under parallel lanes the main
    thread's result prints landed in whichever in-process check happened to hold the redirect and
    vanished when its buffer was read back -- a parallel run printed 50 of 162 rows, no trailer, no
    verdict line, and a red row could be among the missing (2026-09-22). The exit code was right the
    whole time; the log was not. --jobs 1 never showed it (one lane, nothing to interleave)."""

    def __init__(self, real):
        self.real = real
        self.bufs = {}

    def _target(self):
        return self.bufs.get(threading.get_ident(), self.real)

    def write(self, s):
        return self._target().write(s)

    def flush(self):
        return self._target().flush()

    def __getattr__(self, name):  # encoding, isatty, fileno, ... -- the real stream's
        return getattr(self.real, name)


def _execute(c):
    if c.fn is not None:
        buf = io.StringIO()
        out = sys.stdout
        try:
            if isinstance(out, _ThreadStdout):
                out.bufs[threading.get_ident()] = buf
                try:
                    r = c.fn()
                finally:
                    out.bufs.pop(threading.get_ident(), None)
            else:
                with contextlib.redirect_stdout(buf):
                    r = c.fn()
            c.ok = r is not False
        except Exception as e:
            c.ok = c.advisory  # an advisory reporter must never break the gate; a check must
            buf.write("[%s] %s -- %s\n" % ("ok" if c.advisory else "FAIL", c.name, e))
        c.text = buf.getvalue().rstrip("\n")
        return
    r = subprocess.run(c.cmd, cwd=c.cwd, capture_output=True, text=True)
    c.ok = r.returncode == 0
    lines = ["[%s] %s" % ("ok" if c.ok else "FAIL", c.name)]
    if not c.ok:
        out = (r.stdout + r.stderr).strip()
        if out:
            lines.append("      " + "\n      ".join(out.splitlines()[:20]))
    c.text = "\n".join(lines)


def run_queued(jobs=None):
    """Execute the queue over its lanes and print every result in DECLARATION order.

    Returns the gate verdict. A check whose driver raises is reported FAILED rather than skipped --
    a lint that can lose a check silently is the failure mode this whole file is written against.
    """
    lanes = {}
    for c in _QUEUE:
        lanes.setdefault(c.lane, []).append(c)

    def drive(lane):
        for c in lane:
            try:
                _execute(c)
            except Exception as e:
                c.ok = False
                c.text = "[FAIL] %s\n      driver error: %r" % (c.name, e)
            finally:
                c.done.set()

    ok = True
    real = sys.stdout
    sys.stdout = _ThreadStdout(real)
    try:
        with ThreadPoolExecutor(max_workers=jobs or min(len(lanes), (os.cpu_count() or 4))) as ex:
            for lane in lanes.values():
                ex.submit(drive, lane)
            for c in _QUEUE:
                c.done.wait()
                if c.text:
                    print(c.text, flush=True)
                if not c.advisory:
                    ok &= c.ok
    finally:
        sys.stdout = real
    return ok


def run_chunked(name, cmd_prefix, files, cwd=REPO):
    """Like run(), but split `files` across several invocations so the assembled command line stays
    under Windows CreateProcess's ~32767-char limit.

    The tree crossed that ceiling for the first time in this session (498 files, ~33.9K chars of
    argv) -- clang-format's own file-list argument, not a wrapper shell, so xargs-style batching has
    to happen here rather than being solved by quoting. A fixed per-call BUDGET (chars, not a file
    count) is what keeps this correct as individual paths get longer or shorter, rather than a count
    that silently stops being enough once average path length grows.
    """
    BUDGET = 20000  # chars of joined file list per invocation; well under the ~32767 OS ceiling
    ok = True
    outputs = []
    batch = []
    batch_len = 0
    batches = []
    for f in files:
        add = len(f) + 3
        if batch and batch_len + add > BUDGET:
            batches.append(batch)
            batch, batch_len = [], 0
        batch.append(f)
        batch_len += add
    if batch:
        batches.append(batch)

    # The batches exist only because of an argv-length ceiling -- one logical check over disjoint
    # file sets, with nothing ordered between them, so they fan out.
    def fmt(b):
        return subprocess.run(cmd_prefix + b, cwd=cwd, capture_output=True, text=True)

    with ThreadPoolExecutor(max_workers=min(len(batches), (os.cpu_count() or 4))) as ex:
        for r in list(ex.map(fmt, batches)):
            if r.returncode != 0:
                ok = False
                out = (r.stdout + r.stderr).strip()
                if out:
                    outputs.append(out)
    print(f"[{'ok' if ok else 'FAIL'}] {name} ({len(batches)} batch(es))")
    if not ok and outputs:
        joined = "\n".join(outputs)
        print("      " + "\n      ".join(joined.splitlines()[:20]))
    return ok


def rust_row(mode, label):
    """A tools/lint_rust.py row, with its THREE-VALUED verdict preserved (dist DS1).

    Every other row here is a boolean: a subprocess exits 0 or it does not. The Rust rows cannot be,
    because cargo is OPTIONAL on a machine -- it arrives via rustup, which nothing else in this tree
    needs -- and both boolean answers are wrong. Failing makes the gate permanently red on any
    machine that does not build the launcher; passing makes "cargo fmt never ran" look exactly like
    "cargo fmt found nothing", which is the VACUOUS PASS the --ci table above calls out by name.

    So lint_rust exits 3 for "did not run", and this wrapper renders that as a `[SKIP]` line that
    NAMES the reason and the remedy, without failing the gate. Nothing else in the roster gets to do
    this: a row is allowed a skip state only when the absent thing is a per-machine toolchain rather
    than a repo file, and when the skip line says so where a reader of the log will see it."""

    def run():
        r = subprocess.run(
            [sys.executable, os.path.join(REPO, "tools", "lint_rust.py"), mode],
            cwd=REPO,
            capture_output=True,
            text=True,
        )
        out = (r.stdout + r.stderr).strip()
        if r.returncode == 3:
            first = out.splitlines()[0] if out else "cargo not found"
            print("[SKIP] %s -- DID NOT RUN: %s" % (label, first))
            return True
        print("[%s] %s" % ("ok" if r.returncode == 0 else "FAIL", label))
        if r.returncode != 0 and out:
            print("      " + "\n      ".join(out.splitlines()[:20]))
        return r.returncode == 0

    return run


def declare_checks(args):
    """Queue the whole roster. Declaration is separated from execution so --ci-selftest can inspect
    the roster (and the skip list against it) without running a single check."""
    files = list(cxx_files())
    # THE DRIFT GATES' OWN BLIND SPOT. Every --check below compares a GENERATED HEADER against
    # tools/data/dll_call_protos.json -- so a stale json is not drift to any of them, it is the
    # input they all faithfully agree with, and the chain goes green while the DB has moved on.
    # Measured 2026-08-30: the protos dump had missed three renames committed sessions earlier, and
    # it surfaced only because a human regenerated the file for an unrelated reason. This gate
    # cross-checks the DB dumps against EACH OTHER (they are refreshed on different schedules, so a
    # partial refresh shows up as disagreement) and caught a live stale en_addr_index.json on its
    # first run. Offline and sub-second on purpose -- see the script header for what it cannot see.
    check(
        "dump coherence (dll_call_protos vs the function dumps)",
        [sys.executable, os.path.join(REPO, "tools", "lint_dump_coherence.py")],
    )
    check(
        "dump coherence -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_dump_coherence.py"), "--selftest"],
    )
    check(
        "addr-drift (gen_dll_addrs --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_dll_addrs.py"), "--check"],
    )
    check(
        "struct-drift (gen_dll_structs --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_dll_structs.py"), "--check"],
    )
    check(
        "calls-drift (gen_dll_calls --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_dll_calls.py"), "--check"],
    )
    check(
        "exports-drift (gen_dll_exports --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_dll_exports.py"), "--check"],
    )
    check(
        "region-mirror (HASH_REGIONS[] vs mp_analyze REGION_NAMES)",
        [sys.executable, os.path.join(REPO, "tools", "lint_region_mirror.py")],
    )
    # DET-FLAKE. The determinism gate's own verdict logic, over synthetic peer logs -- no rig, no
    # game, ~1 s. Both failure directions are arms: a genuine desync must never be excused as
    # environmental (2, 5), and agreement over a match that died early must never pass (4). It is
    # here rather than in the rig gate precisely because the rig gate is the thing it protects.
    check(
        "det-guard (mp_analyze --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "mp_analyze.py"), "--selftest"],
    )
    # TACT1-P C1. The liveness census is what stops a promotion item closing over a live set two
    # orders of magnitude smaller than the one it claims (G106: SIM1-P and TACT1-P both closed green
    # over 4 and 11 rows). Gated here because the clause asks for a GENERATED, DRIFT-GATED census
    # and an ungated generator is a snapshot -- the committed JSON drifts the moment a rebind row or
    # a ship key moves, and a stale census reads exactly as authoritative as a fresh one. `--check`
    # also fails on an unexplained NOT-LIVE row, so a body that quietly falls off the live set fails
    # the lint rather than waiting to be noticed.
    # The coverage-baseline comparison is REGRESSION-ONLY, so its easy bug is a rule that quietly
    # never fires. This runs its six arms offline -- no collector, no rig, no build -- so the check
    # that would catch a scenario losing reach is itself checked on every lint.
    check(
        "coverage-drift arms (coverage --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "coverage.py"), "--selftest"],
    )
    # ---- THE PROMOTED-vs-ORIGINAL A/B SELF-CHECK ROW DROPPED AT FORK F5M S4b (the archive-tool cut) ----
    # It gated the promoted-vs-original differential's OWN negative cases -- the ways that A/B could
    # report a green meaning nothing: a control that does not really roll the key back, a fixture
    # whose poke lands past the end of the run (so the go-red arm blames a working oracle), and two
    # scenarios sharing one golden. Keep the finding, drop the row: the differential itself is the
    # migration era's oracle -- it compares OUR body against the ORIGINAL body inside a retail game
    # -- the gate-driver unit that ran it goes with this row, and the public tree carries no retail
    # binary to compare against. What proves our bodies there is the libref replay against recorded
    # goldens, the lockstep determinism pair, and the selftests.
    # LIB-VA0. libmh may not GAIN a call into original code at a fixed VA, and every site it still
    # has must name the item that will remove it. The ratchet is the point: R9's direct-site class
    # sat unowned for five days behind a number that fell on its own as translation proceeded, which
    # is exactly what an unowned measure looks like while it is still moving. Fails in BOTH
    # directions -- a rise is a regression, a fall is a stale baseline to re-record -- because a
    # ratchet that only fails upward becomes a ceiling nobody lowers. Pure: no rig, no build.
    check(
        "libmh VA-call ratchet (gen_va_census --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_va_census.py"), "--check"],
    )
    check(
        "libmh VA-call ratchet -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "gen_va_census.py"), "--selftest"],
    )
    # LIB-VA0 clause 5's LINK-PROOF sibling. gen_va_census measures source spelling and can be fooled
    # by a VA that never appears as an `mh::call::` token -- LIB-CRT found four of those (the qsort
    # comparators, passed as `(void *)mh::addr::X` DATA) inside a clause asserting there were none.
    # This one reads the built lib's executable sections, where every VA is a bare immediate however
    # the source spelled it. SKIPS when libmh is not built, so a fresh clone does not fail on it.
    check(
        "libmh object-byte VA ratchet (scan_libmh_vas --check)",
        [sys.executable, os.path.join(REPO, "tools", "scan_libmh_vas.py"), "--check"],
    )
    # LIB-REF-IN. The INBOUND surface's membership is DERIVED, and this is what keeps it derived.
    # The failure it guards is the one docs/libmh-abi.md section 8 already demonstrated: a hand table
    # that read as authoritative and could not be re-derived (its own 41/88/111 does not reproduce
    # even at its own commit). So the tool re-walks the ledgers and the call graph on every run and
    # refuses on a candidate row with no disposition, a caller with no class, an adjudication row
    # whose body is no longer a candidate, an entry a row derives that the hand header does not
    # declare, a declaration no row derives, an order parameter that cannot ride an int32_t argv,
    # and a stale generated output. Pure: no rig, no build, no Ghidra.
    check(
        "libmh inbound-surface drift (gen_libmh_inbound --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_libmh_inbound.py"), "--check"],
    )
    # CRT-X87. Inline x87 in a translated body is unportable AND invisible to every offline oracle,
    # and it had been silently duplicated -- one nine-instruction sequence existed 26 times under six
    # different local names. A ratchet rather than a ban: blocks outside mh/fp/ may exist, the count
    # may not rise, and it may not sit stale after it falls.
    check(
        "x87 inline-asm ratchet (lint_x87_asm --check)",
        [sys.executable, os.path.join(REPO, "tools", "lint_x87_asm.py"), "--check"],
    )
    check(
        "x87 inline-asm ratchet -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_x87_asm.py"), "--selftest"],
    )
    # CRT-X87-CPP's done_when, made mechanical: every helper in mh/fp/ is C++ WITH A NAMED fptest
    # case or assembly WITH A RECORDED REASON, and which one is read off the body rather than off the
    # prose. The ratchet above counts asm blocks; this one asks whether each survivor has a reason
    # that still exists. Both arms matter -- this item found two helpers whose recorded refusal was
    # inherited from the shape rather than measured, and was wrong for both.
    check(
        "mh/fp helper states declared + checked (lint_fp_helpers)",
        [sys.executable, os.path.join(REPO, "tools", "lint_fp_helpers.py")],
    )
    check(
        "mh/fp helper states -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_fp_helpers.py"), "--selftest"],
    )
    # THE TWO LIVENESS ROWS WERE DROPPED AT F2D, ahead of the tool itself. gen_liveness_census
    # attributed each row to its owning TU through the MH_SHADOW_REPLACE bridge, which went with the
    # differential oracle -- and without a TU the ours-rooted test credits a function's calls to
    # ITSELF, so the census reported three self-calling sim rows as live and printed 100%. The gate
    # is not merely stale, it is uncomputable, and a gate that cannot be computed must not be green.
    # The tool now refuses rather than publishing; the F2 umbrella's Q4 ruling deletes it at F2E.
    check(
        "save-table-drift (gen_save_table_header --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_save_table_header.py"), "--check"],
    )
    check(
        "patches-drift (gen_dll_patches --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_dll_patches.py"), "--check"],
    )
    # F1E (2026-09-12). The compiled-in form of an mhpatch manifest. Drift-gated because the header
    # is the manifest -- including the recovered "which dwords are relative" table, which is what
    # lets the cave be placed anywhere. An edited manifest whose header was not regenerated would
    # apply the OLD bytes and the parity check would then be comparing the new file against them.
    check(
        "inmem-manifest drift (gen_inmem_manifest --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_inmem_manifest.py"), "--check"],
    )
    # F4C (2026-09-13). The row above gates the ONE compiled-in header; this one gates the verdict
    # over the WHOLE src/patcher corpus -- 40 manifests, 46,294 sites -- because that is where the
    # `abs32_to_section` classification actually has to be total. Deterministic re-derive (every
    # input is in-tree) compared against the committed extract tools/data/inmem_manifest_audit.json,
    # so a manifest edit, a generator change, OR an applier header change that closes/opens a
    # prerequisite all show as a reviewable diff rather than as a silent reclassification.
    check(
        "inmem-manifest corpus classification (gen_inmem_manifest --check-audit)",
        [sys.executable, os.path.join(REPO, "tools", "gen_inmem_manifest.py"), "--check-audit"],
    )
    # ...and the refusal arms, on planted negatives in a temp tree (the check_fork_f2_drop house
    # pattern). A classifier whose refusals nobody has seen fire is not evidence: the arms here
    # include SUPPRESSING an emitted ref to prove the totality assertion is not vacuous -- the
    # assertion that caught `grand_all_caphike_storagecap`'s 15 unseen `[reg + abs32]` cave refs.
    check(
        "inmem-manifest refusals still fire (gen_inmem_manifest --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "gen_inmem_manifest.py"), "--selftest"],
    )
    # F4C-COMP (2026-09-13). The COMPOSITION census: how much of the static-patch surface lands
    # inside a body mh.dll already claims. The fork's Q6 ruling rests on that number, and both of
    # its inputs move independently -- a manifest edit changes the sites, an MH_EXPORT_REPLACE or a
    # new install_trampoline changes the claimants -- so "24% of 46,294" is exactly the kind of
    # figure that goes stale in prose while still being quoted. Re-derived from committed inputs,
    # compared byte-for-byte against tools/data/inmem_composition_census.json.
    check(
        "inmem composition census is current (check_inmem_composition --check)",
        [sys.executable, os.path.join(REPO, "tools", "check_inmem_composition.py"), "--check"],
    )
    # ...and its own negatives, because a census is a COUNT: a broken parser and a genuinely small
    # overlap are the same green. The arms drive strict containment at both body edges, REFUSE a
    # generated header whose table the parser cannot find (an empty class P would read as "no
    # overlap"), and fire the install-site resolver over a tree where every answer is known.
    check(
        "inmem composition census -- the negative cases still fire (--selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_inmem_composition.py"), "--selftest"],
    )
    # F4C-GATE (2026-09-13). THE PARITY ORACLE'S OFFLINE ARM, and it is here because its absence is
    # a filed trap: check_inmem_patch_parity's only arm needed the rig, so when F4C renamed a helper
    # out from under it the tool was dead for a day with every lint row above green (dead-ends
    # G180). This runs the whole comparison -- reference build, ref re-derivation, the baked-value
    # cross-check against mhpatch's own output, the completeness pass -- over every shipped manifest
    # with no rig, and then plants mutations and requires each to go RED. It also re-proves, every
    # run, that all three shipped manifests still apply cleanly to the real clean EN exe: mhpatch
    # refuses on the first failed expected-bytes guard, so a manifest that drifted off the binary
    # cannot reach a lane to find out.
    check(
        "inmem parity oracle, off-rig (check_inmem_patch_parity --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_inmem_patch_parity.py"), "--selftest"],
    )
    # X-TOMB (2026-09-01). The tombstone DEAD-body table -- every function a migration ledger
    # adjudicated `dead`, joined to its en_functions extent. Drift-gated for the same reason as the
    # patch table above: the moment a ledger marks a new row dead, the instrument must arm over it or
    # the claim goes unchecked. The generator also FAILS on a dead row whose ledger addr disagrees
    # with the en_functions entry, so a stale ledger cannot ship a mis-aimed fill.
    check(
        "tombstones-drift (gen_tombstones --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_tombstones.py"), "--check"],
    )
    # FP-FLAGS (2026-09-01). Every mh/sim and mh/ai TU is x87 (/arch:IA32 /fp:precise) in BOTH the
    # DLL and the offline-oracle vcxproj. MSVC x86 defaults to SSE2, and a sim/AI body compiled SSE2
    # diverges from the x87 original where a long trajectory compounds it (reimpl-plan §6). A new
    # sim/AI TU that lands in neither the flagged set nor tools/data/fp_integer_only.json fails here,
    # so the landmine cannot re-enter silently -- the check that made the 99-unflagged-AI gap visible.
    check(
        "fp-flags (lint_fp_flags)",
        [sys.executable, os.path.join(REPO, "tools", "lint_fp_flags.py")],
    )
    # O4-0 (2026-08-27). The order-ISSUE oracle's GOLDEN -- per call site, the four container
    # arguments and the scratch writes, extracted from the original's disassembly as expressions over
    # each wrapper's parameters. Drift-gated for a reason the other generated headers do not share:
    # it is the EXPECTATIONS `libmh_selftest.exe issuetest` compares against, so a stale or edited golden
    # does not make the suite fail, it makes the suite agree with whatever the code now does. The
    # generator also cross-checks itself against the independent pcode reading in
    # order_matrix_raw.json and exits non-zero on disagreement, so this gate covers both.
    check(
        "order-issue golden drift (gen_order_issue_golden --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_order_issue_golden.py"), "--check"],
    )
    # SIM1-DISPATCH. The (state -> handler) pairing our registrar installs into the two dispatch
    # tables, extracted from llm_strat_register_state_handlers' own disassembly. Same drift gate as
    # the save block table above and for the same reason: it is 78 assignments, a wrong one gives a
    # unit state another state's behaviour, and nothing else in the tree would notice -- the wrong
    # handler is still a handler. Ghidra-free (it compares the header against the committed JSON).
    check(
        "dispatch-table drift (gen_state_handler_table --check: state handlers + bldg-type callbacks)",
        [sys.executable, os.path.join(REPO, "tools", "gen_state_handler_table.py"), "--check"],
    )
    # ---- THE LEDGER-EVIDENCE ROWS DROPPED AT FORK F5M S4b (the archive-tool cut) ----------------
    # They gated the migration ledgers' evidence: a terminal row must carry a written reason, every
    # oracle it names must compile and run, and a `verified` row whose shadow site made ZERO calls
    # must cite something. The finding worth keeping is why it was needed at all -- the `-V` debt
    # rule keyed on `state == "reviewed"`, so a row promoted straight to `verified` on thin evidence
    # was invisible to it and to every terminal-state count: the tier recorded the doubt and nothing
    # gated on the tier. Its subject is the migration ledger, which the fork freezes as history.
    # Host-global leases that serialize the shared singletons (Ghidra writes, the rig) between the
    # main tree and a worktree loop. Its selftest exercises acquire/timeout/want/dead-PID+sticky reap.
    check(
        "host-global lease acquire / reap / want (hostlock --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "hostlock.py"), "--selftest"],
    )
    # tooling:TL-SUITE-TEARDOWN -- the shared kill-on-close job-object helper every local child this
    # rig launches now goes through. Offline, no rig, no game (~2.5 s measured): an ARM child dies
    # within seconds of its job-assigned parent's ABRUPT os._exit(0) (no cooperation, exactly what a
    # per-test timeout does to a runner) while a MUTATION control with no job assigned survives,
    # proving the ARM result is not vacuous; and a file held open by a real child process is named
    # in copy_or_refuse's REFUSAL rather than raised as a bare PermissionError.
    check(
        "kill-on-close job assignment + copy-refusal fallback (win_job --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "win_job.py"), "--selftest"],
    )
    # ---- THE GHIDRA-SIDE WRITE-LEASE ROW DROPPED AT FORK F5M S4b (the archive-tool cut) ---------
    # It exercised the Jython/PyGhidra half of the ghidra-write lease (acquire / timeout / reap) in
    # the Py2/3 subset the in-Ghidra write scripts are limited to. The host-global lease row above
    # STAYS: that is the CPython half, and it is the one the rig and the worktree loop serialise on.
    # The Ghidra-side half went with the database pipeline, which the public tree does not carry.
    # ST1. The region registry is the ONE derivation of where state lives; the five manifests that
    # used to answer that independently are views over it. `--check` is the ordinary drift gate, and
    # it also refuses when a save block names an address the COMMITTED registry does not cover --
    # which is the arm that can actually go red, since a --refresh would merge the save table in and
    # could never disagree with itself.
    check(
        "state-registry drift + save-block coverage + ownership interlock (gen_state_registry --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_state_registry.py"), "--check"],
    )
    check(
        "state-registry coverage + ST3 ownership interlock -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "gen_state_registry.py"), "--selftest"],
    )
    # F1B. NET-SESSION's ADDRESS TABLE, read out of the retail bytes instead of out of a live run.
    # `netsessiontest` proves our body writes the right values into a fixture; it cannot prove the 22
    # pointers name the addresses the ORIGINAL writes. That second half used to need a rig and two
    # boots (the read-back probe + check_netprobe.py -- both deleted at fork F2F), which is why it
    # was evidence collected once rather than a gate; this disassembles
    # llm_net_session_globals_reset, folds its 28 absolute
    # stores into 22 fields, and requires each to be a region BASE holding the constant net_session.h
    # declares. The extraction-totality arm is the one that makes the rest mean anything -- an
    # unclassified instruction REFUSES, because a partial table is right about every row it has and
    # silent about the row it dropped. Offline and ~0.1 s; it is the SOLE proof of the address
    # table now that the probe is gone.
    check(
        "NET-SESSION address table vs the retail bytes (check_net_session_addrs --check)",
        [sys.executable, os.path.join(REPO, "tools", "check_net_session_addrs.py"), "--check"],
    )
    check(
        "NET-SESSION address table -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "check_net_session_addrs.py"), "--selftest"],
    )
    # F1A, FLIPPED TO THE REAL CLAIM AT F3D. Ruling D1 wants config (1) to carry zero references
    # from net seam code into the lockstep closure once desync_watch moves to mh.dll. Until F3D the
    # residue was non-empty and being drained item by item, so this row ran `--baseline`, which
    # pinned the exact surviving set in both directions (a new coupling failed, and so did a removal
    # that did not shrink the literal in the same commit). F3D drained it -- desync_watch moved out
    # of mh/lockstep, R2/R3 went behind `mh::config::ours_run()`, R4's clamp relocated to
    # mh/fix/resync_clamp.h, R5's `[net] fix_audit` reader was deleted -- so the row now asserts the
    # claim itself: ANY config-(1) residue is a failure.
    #
    # Exit 2 still doubles as "the tool is broken": four known couplings must be re-found on every
    # run (SELFTEST, re-picked at F3D since three of the old anchors were symbols F3D removes), and
    # the extracted instrument's reach back into the closure must still measure empty -- so a
    # neutered scan can never read as a clean zero. --src-only needs no build artifacts; the OBJ
    # mechanism is exercised by hand against a fresh Debug build, since lint does not build.
    check(
        "net-seam -> lockstep coupling: ZERO config-(1) residue (check_net_lockstep_refs "
        "--require-empty)",
        [
            sys.executable,
            os.path.join(REPO, "tools", "check_net_lockstep_refs.py"),
            "--require-empty",
            "--src-only",
        ],
    )
    # F2F. Fork ruling D8's gate, in its POST-DROP shape: the deferred-effect machinery stays
    # deleted. It was a positive list of adjudicated mh::effects:: call sites while the machinery
    # existed (F1C -- and that list is what caught `[net] sync_gameover` riding a gate with no
    # fallback); with the module gone it is a ZERO-SURVIVOR scan of src/ + tools/. A zero-survivor
    # scan cannot anchor itself on the real tree, so its controls live in --selftest: each regex is
    # re-fired at a planted violation in a temp tree, and an empty tree is REFUSED rather than read
    # as clean.
    check(
        "fork D8 closure: the deferred-effect machinery stays deleted (check_fork_d8)",
        [sys.executable, os.path.join(REPO, "tools", "check_fork_d8.py")],
    )
    check(
        "fork D8 closure -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "check_fork_d8.py"), "--selftest"],
    )
    # F2E. The fork's WHOLE drop-vocabulary gate, of which D8's is the effects-shaped half: the
    # per-row configuration surface F2D/F2F/F2E deleted stays deleted. Same zero-survivor shape and
    # the same controls (plant-in-a-temp-tree reds through the real walker, plus a walker-liveness
    # arm), and it also asserts the ALLOWED survivors stay GREEN -- the arm-log's `; [promote]` /
    # `; [rebind]` prefixes, MH_LIBMH_BIND, mh::rebind::armed, MF_MEASURED -- because a gate that
    # also reds on what the ruling KEPT is not stricter, it is a lint row someone satisfies by
    # deleting a live mechanism.
    check(
        "fork F2 drop: the per-row configuration vocabulary stays deleted (check_fork_f2_drop)",
        [sys.executable, os.path.join(REPO, "tools", "check_fork_f2_drop.py")],
    )
    check(
        "fork F2 drop -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "check_fork_f2_drop.py"), "--selftest"],
    )
    # F4H. LANE NUMBERS ARE DISJOINT. A lane number is the game's machine-wide single-instance mutex
    # ("MHMutNN"), so two lanes carrying one number cannot both run -- the second game dies inside
    # retail's own single-instance check with no window, no frame and no log line, and what the
    # runner can say about it is `did not present a frame within 60s`. That is not a hypothetical:
    # the capture suite's block is DERIVED from its registry, it grew 20 -> 36 lanes, and it walked
    # onto ui_soak's 32, --sp-determinism's 31 and the tactical lanes' 33..36 while every hand-picked
    # constant sat under a comment asserting it was clear of them. Six overlapped gate reds came out
    # of it and were diagnosed twice as CPU starvation.
    #
    # This row is HERE, in lint, because the failure it stops is a REGISTRY EDIT -- adding a scenario
    # -- and lint is the only gate that runs on an edit. It reads the live TESTS registry, so the
    # next scenario past the block's capacity is a lint failure naming both numbers.
    check(
        "fork F4H lane allocation: blocks disjoint, suite demand inside its block (lane_alloc)",
        [sys.executable, os.path.join(REPO, "tools", "lane_alloc.py"), "--check"],
    )
    check(
        "fork F4H lane allocation -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lane_alloc.py"), "--selftest"],
    )
    # GATE DIET (2026-09-24). COST IS A VERDICT: every capture-suite row carries `budget_s` (its
    # expected seconds under gate load) and a row over 120 s says why in `long_why`, because
    # run_gate.py reds a scenario that runs past 1.5x its budget and a row with no budget cannot be
    # judged. Here, in lint, for the lane row's reason: the failure is a REGISTRY EDIT (a new row
    # with no budget), and lint is what runs on an edit. The second row keeps run_gate's own red
    # rules honest (a planted over-budget scenario, suite and gate each go red; a SKIP does not).
    check(
        "gate diet: every TESTS row has budget_s, long rows a long_why (test_ui --check-budgets)",
        [sys.executable, os.path.join(REPO, "tools", "test_ui.py"), "--check-budgets"],
    )
    # TL-SUITE-REGDATA: registry.yaml is schema-checked on every load; this keeps the refusals honest.
    check(
        "scenario registry schema -- unknown key / wrong type / missing key refused (ui_registry)",
        [sys.executable, os.path.join(REPO, "tools", "ui_registry.py"), "--selftest"],
    )
    check(
        "gate diet: run_gate's cost red rules -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "run_gate.py"), "--selftest"],
    )
    # F5A. THE PUBLISHABILITY LEDGER. `tools/data/publish_ledger.json` gives every path git tracks
    # exactly one disposition, so the public cut is a DERIVED set rather than a judgement call made
    # per file on the day. This row is here, in lint, for the same reason the lane row is: the
    # failure it stops is an ordinary edit -- ADDING A FILE. A new path no rule covers FAILS, so a
    # file cannot arrive in the tree already assumed publishable, which is the only way a ledger
    # like this stays true for more than a fortnight. The same row also refuses a rule that has
    # outlived its files.
    #
    # The --closure row is the F5C contradiction class made cheap: a published source that needs a
    # path the public tree will not carry. It already has one real positive (save_selftest.cpp:21
    # includes the unpublishable LZW fixtures, so the public tree does not compile as specified),
    # which is declared in the ledger and must KEEP reproducing until F5C fixes it -- when it stops,
    # this row goes red until the declaration is deleted. Edges into rows still waiting on a user
    # answer are reported as CONDITIONAL and do not fail: they are the measured cost of a Q3/Q4/Q5
    # "withhold", not a defect.
    #
    # WHICH ARM RUNS IS CHOSEN BY THE TREE, NOT BY --ci (fork F5M). The obvious wiring -- skip these
    # two under --ci -- rests on "--ci means the public tree", and that is FALSE here: .github's
    # workflow is dispatched by hand on this repo's private GitHub remote as well, so --ci
    # runs on the PRIVATE tree too and skipping there covered nothing while costing the row. So the
    # rows ask the ledger which tree this is (check_publishable.is_public_tree, declared by the
    # ledger's own _public_tree_marker) and run the matching arm, naming it in the row. On the
    # private tree that is the pair below, unchanged; on the cut's output it is --public, which
    # asks the MIRROR question -- no withheld path tracked, no publish rule left matching nothing.
    # The private arms cannot run there (every withhold rule matching zero paths is their own
    # dead-rule failure) and the public arm cannot run here, so there is no mode in which a row is
    # silently vacuous.
    _cp = os.path.join(REPO, "tools", "check_publishable.py")
    if _tree_is_the_cut():
        check(
            "fork F5M publish ledger, public tree: nothing withheld leaked (--public)",
            [sys.executable, _cp, "--public"],
        )
        check(
            "fork F5M publish ledger, public tree: no edge into a non-published path "
            "(--public --closure)",
            [sys.executable, _cp, "--public", "--closure"],
        )
    else:
        check(
            "fork F5A publish ledger: every tracked path dispositioned (check_publishable)",
            [sys.executable, _cp],
        )
        check(
            "fork F5A publish ledger: no published source needs a withheld path (--closure)",
            [sys.executable, _cp, "--closure"],
        )
    check(
        "fork F5A publish ledger -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "check_publishable.py"), "--selftest"],
    )
    # F5M S5. THE SEED'S DERIVED VCS CONFIG. The public tree's .gitignore/.gitattributes are not the
    # private ones copied -- they are derived from a per-entry disposition table in
    # tools/build_public_seed.py, which fails closed on an entry it has no answer for. That property
    # is worth nothing unannounced, because the derivation and the file it derives from live in
    # different commits: add an ignore rule here and the seed either carries it undecided or loses
    # it silently. So the snapshots under tools/data/public_repo_config/ are drift-gated like every
    # other generated artifact, and this row is TREE-DISPATCHED for the same reason the two above
    # are -- on the private tree it re-derives and compares to the snapshot, on the cut's output the
    # snapshot's counterpart is the checkout's own config, which the seed installed.
    check(
        "fork F5M public seed: the derived .gitignore/.gitattributes still reproduce (--check)",
        [sys.executable, os.path.join(REPO, "tools", "build_public_seed.py"), "--check"],
    )
    check(
        "fork F5M public seed -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "build_public_seed.py"), "--selftest"],
    )
    # F5E. THE CITATION GATE, the prose/comment counterpart of --closure above. --closure scans
    # BUILD edges (C/C++ includes, Python string constants) and is blind to comments and .md by
    # construction; this is the half that reads them. Three directions, one row: DANGLING (a path
    # that exists nowhere) and PRIVATE-from-published-PROSE are hard, PRIVATE-from-CODE is a
    # per-file tightening-only ratchet in tools/data/citation_baseline.json.
    check(
        "fork F5E citations: no published file cites a path the cut does not carry (--strict)",
        [sys.executable, os.path.join(REPO, "tools", "lint_citations.py"), "--strict"],
    )
    check(
        "fork F5E citations -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_citations.py"), "--selftest"],
    )
    # F5G. THE TOOL TRIAGE, made re-runnable. F0 classified tools/ into KEEP/ARCHIVE/UNSURE as three
    # markdown tables, and within a month the tree had moved under them: 12 scripts deleted at F2D,
    # 20 arrived, and one row said ARCHIVE about a tool this very file gates on every run. The
    # tables could not say so, because prose cannot be re-run. tools/data/tool_dispositions.json is
    # the live answer and this row is what keeps it true -- and again the failure it stops is an
    # ordinary edit (adding or deleting a tool), which is why it belongs in lint rather than a gate.
    #
    # The `keep` half is two-sided on purpose: a keep row must name what RUNS it (a row here, a
    # run_gate unit, a vcxproj step, a workflow, or another keep tool), or declare `hand_run` --
    # and `hand_run` is refused on a tool something does invoke, so the marker cannot decay into a
    # blanket exemption. It deliberately stops at tools/; see the file's docstring for why a
    # tools/data reachability lint would be dishonest (the f-string-addressed sim_resid_* files).
    check(
        "fork F5G tool triage: every tools/*.py dispositioned, every keep row invoked "
        "(check_tool_dispositions)",
        [sys.executable, os.path.join(REPO, "tools", "check_tool_dispositions.py"), "--check"],
    )
    check(
        "fork F5G tool triage -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "check_tool_dispositions.py"), "--selftest"],
    )
    # F5M S4b. THE ARCHIVE CUT, kept honest. The user ruled that the archive class is WITHHELD from
    # the public tree rather than deleted from the repo, so the cut is one publish-ledger row whose
    # globs enumerate those tools -- and an enumeration is a COPY of the class above. This row is
    # what stops the copy rotting, in both directions: an archive tool missing from the rule SHIPS
    # in the seed silently, and a keep tool listed in the rule is CUT OUT of the public tree, which
    # is a red public CI (the S4a re-triage found eleven tools in that second state, four of them
    # modules the public lint imports). Also asserts the rule sits ahead of `tools-tree`, because
    # the ledger is first-match and behind it the carve-out matches nothing.
    check(
        "fork F5M archive cut: the ledger withholds exactly the archive class "
        "(check_tool_dispositions --ledger)",
        [sys.executable, os.path.join(REPO, "tools", "check_tool_dispositions.py"), "--ledger"],
    )
    # TL-CI1. THE RELEASE PACKAGER's rules, which are the part of a release that goes wrong
    # silently: a zip carrying the STANDALONE libmh.dll instead of the hosted one gives the user a
    # configuration they did not ask for and cannot see, and a zip named after a tag whose binaries
    # were never stamped with it is a release no bug report can be traced back to. The selftest is
    # hermetic -- a temp tree, fake artifacts, no toolchain and no game -- so it runs here and on
    # the CI runner, and it asserts the three zips' EXACT file lists, that the ship and debug inis
    # differ only in the named diagnostic keys, that SHA256SUMS verifies and goes red on a tampered
    # zip, and every refusal. It additionally exercises the VERSIONINFO reader and the
    # hosted-vs-standalone discrimination against real PEs WHEN a Release tree happens to exist,
    # and prints a note rather than passing quietly when it does not.
    check(
        "TL-CI1 release packaging: file lists, ini variants, sums and refusals "
        "(release_package --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "release_package.py"), "--selftest"],
    )
    # 2026-09-20 (user ruling). THE ini's STRING KEYS CARRY NO SAME-LINE COMMENT. Win32
    # GetPrivateProfileStringA returns the rest of the line as the value, so `probe_text=   ; empty
    # = off` drew that comment on every present of a STOCK install and `relay=HOST:PORT   ; UDP
    # only...` dialled the comment (2026-09-19). Prose warnings on three lines did not stop the
    # fourth, so the rule is a gate: the string-key set is DERIVED from the DLL's own
    # GetPrivateProfileStringA calls (a new string key is covered the moment its reader exists),
    # and any live or commented-out line setting one of them with a `;` after the `=` is red.
    # Integer keys keep their comments (atoi stops at the `;`).
    check(
        "ini: no string-valued key in mh_net.example.ini carries a same-line comment "
        "(lint_ini_string_keys)",
        [sys.executable, os.path.join(REPO, "tools", "lint_ini_string_keys.py")],
    )
    check(
        "ini string-key lint -- a planted same-line comment (live and commented-out) goes RED",
        [sys.executable, os.path.join(REPO, "tools", "lint_ini_string_keys.py"), "--selftest"],
    )
    # ---- dist RP3: the report drain (BEGIN) -------------------------------------------------
    # RP3's drain tool pulls report directories off the VPS collector (dist:RP2) over a
    # `restrict,command="rrsync -ro ..."` SSH key. The selftest is OFFLINE -- it rsyncs local
    # directory to local directory (no VPS, no network) -- so it runs here and on the CI runner:
    # a report tree drains in exactly once (a second consecutive drain transfers zero new files),
    # an incomplete report directory (no meta.json yet, mid-atomic-write on the server) is never
    # summarized, and every refusal (no VPS_HOST, no key, no rsync binary) fires with a clear
    # message instead of a stack trace. The live VPS half (two real drains, the rrsync push
    # refusal, crash_report.py naming a function from a drained report) is operator-run per
    # src/collector/README.md's "Draining reports" section -- it needs a real VPS + a real key,
    # neither of which a CI runner has.
    check(
        "dist RP3 report drain: pull-once, incomplete-report skip, refusals "
        "(drain_reports --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "drain_reports.py"), "--selftest"],
    )
    # ---- dist RP3: the report drain (END) ---------------------------------------------------
    # dist LA2. THE UPDATE MANIFEST's signer. The launcher verifies `manifest.json` against one
    # compiled-in minisign key before it parses a byte of it, so the whole update path rests on this
    # tool producing signatures that the `minisign-verify` crate accepts -- and on it refusing to
    # publish a manifest whose digests do not match the zips the packager actually built. The
    # selftest is hermetic (a temp tree, fake zips, no network and no key on disk) and its first
    # assertions are RFC 8032's own published Ed25519 test vectors, which is what makes the
    # stdlib-only curve arithmetic in that file checked against the standard rather than against
    # itself. It then round-trips a generated key through the minisign key/signature formats, proves
    # a flipped manifest byte, a rewritten trusted comment, a foreign key and a truncated signature
    # are all caught, and proves every build refusal fires.
    check(
        "dist LA2 update manifest: RFC 8032 vectors, the minisign formats, the tampers and the "
        "refusals (gen_update_manifest --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "gen_update_manifest.py"), "--selftest"],
    )
    # F3C. D5's boundary: the determinism harness arms ONLY through the named hook points, never
    # through the raw inline-detour primitives (nor a hand-rolled VirtualProtect, which is how the
    # one site the 77-site measurement missed wrote its bytes). Two-sided on purpose -- zero
    # raw-primitive callers AND a floor on arms through the API, because a zero-survivor scan over
    # code that arms nothing is not evidence (G106). Same plant-in-a-temp-tree selftest as the F2
    # rows above.
    check(
        "fork D5 hooks: harness-owned code stays off the raw primitives (check_fork_d5_hooks)",
        [sys.executable, os.path.join(REPO, "tools", "check_fork_d5_hooks.py")],
    )
    check(
        "fork D5 hooks -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "check_fork_d5_hooks.py"), "--selftest"],
    )
    # F4A. THE MODULE-BIND gate's offline half. The live half is the `module_absent` suite scenario
    # (a lane that asks mh.dll to LoadLibrary a sibling and is given no file), and like check_arm_order
    # it needs a RUN DIRECTORY, which lint must not acquire -- so lint proves the INSTRUMENT and the
    # suite runs the gate. The selftest is planted logs: an outcome with no arm end marker (i.e. the
    # boot died), an end marker BEFORE the outcome, the wrong outcome for the expectation, a bind
    # whose call-through is broken, the two no-transport outcomes swapped for each other, and a log
    # with no `[modules]` line at all all go RED. That last one is the row that matters most: since
    # F4B the bind is UNCONDITIONAL, so a build that quietly stopped binding would otherwise pass on
    # an unchanged main-menu frame.
    check(
        "fork F4A module bind -- the negative cases still fire (check_module_bind --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_module_bind.py"), "--selftest"],
    )
    # F4A's OTHER done_when clause -- "every satellite DllMain proven inert" -- as a gate rather than
    # a paragraph. With one satellite the rule is provable by reading it; with four (mh_net, libmh,
    # mh_harness and whatever follows) it is a rule somebody has to remember, which is the G106
    # hand-list shape. So it is DERIVED: every DllMain in src/ is found, exactly one is exempt by
    # name (mh.dll's -- the sole orchestrator, and F4A's ruling is that there is exactly one), and
    # every other body may call nothing but DisableThreadLibraryCalls / GetCurrentThreadId /
    # QueryPerformanceCounter. A satellite that opens a log, installs a hook or calls LoadLibrary
    # from DLL_PROCESS_ATTACH reds here rather than at the next 0xC0000409 nobody can reproduce.
    check(
        "fork F4A satellites: every DllMain but the orchestrator's is inert (--dllmain-inert)",
        [sys.executable, os.path.join(REPO, "tools", "check_module_bind.py"), "--dllmain-inert"],
    )
    check(
        "fork F4A satellite DllMain rule -- the negative cases still fire",
        [
            sys.executable,
            os.path.join(REPO, "tools", "check_module_bind.py"),
            "--dllmain-inert",
            "--selftest",
        ],
    )
    # F4B. THE BOUND SURFACE. mh.dll's only way into mh_net.dll is the forwarding shims over the
    # GetProcAddress table, and this asserts the four statements that keep that true: the symbol
    # table and mh_net.def agree in BOTH directions, every MH_Net_* the header declares has a row
    # (and therefore a shim and a defined absent value), every MH_Net_*/MH_Key_* call in mh/
    # resolves to a row, and the two module_bind TUs stay one-per-project so the shims and the real
    # transport bodies can never land in one image.
    #
    # It is what F4B's "the 18-site list is 0 ungated, committed as a check" became. The 18 ungated
    # call sites were not fixed with 18 guards -- the SURFACE carries absence now, so ungated is not
    # a state a call site can be in. A grep for guards would therefore measure nothing and would go
    # green on a tree where the shims had been deleted; this measures the construction instead. The
    # floor (20 rows) is there because a zero-row parse would pass every one of those checks
    # vacuously -- G106, one level down.
    check(
        "fork F4B: mh.dll reaches mh_net.dll only through the bound table (--net-surface)",
        [sys.executable, os.path.join(REPO, "tools", "check_module_bind.py"), "--net-surface"],
    )
    check(
        "fork F4B bound surface -- the negative cases still fire",
        [
            sys.executable,
            os.path.join(REPO, "tools", "check_module_bind.py"),
            "--net-surface",
            "--selftest",
        ],
    )
    # NOT A ROW HERE: `--subset` (mh_net.dll's static imports must be a subset of mh.dll's). It reads
    # IMPORT TABLES, so it needs a build, and lint does not build -- the same division
    # check_net_lockstep_refs draws between --src-only and its OBJ mode. run_gate.py runs it
    # immediately after the Release build, where fresh binaries are guaranteed.
    # F3A. The ARM-LOG ORDER gate -- the instrument every F3 refactor's proof uses. Two rows here,
    # and NEITHER of them is the gate itself: the gate wants a RUN DIRECTORY, which lint does not
    # have and must not acquire (a lint row that launches the game would cost rig minutes on every
    # commit, and the rig is a host-global lease). So lint proves the INSTRUMENT and the refactor
    # items run the gate:
    #
    #   lint  -> `--check-baselines` (the committed templates parse, are non-empty, hold no raw
    #            address or count, and no two steps share a text the matcher could confuse) and
    #            `--selftest` (a planted reorder / insertion / deletion / rewording all go RED
    #            against a temp-tree render of the real baselines, a masked-field change does NOT,
    #            and a log with no end marker is REFUSED rather than read as clean).
    #   F3B..G -> `python tools/check_arm_order.py <run-dir>` on the lane the item's proof ran, and
    #            `--compare <old-run> <new-run>` in any commit that EDITS a baseline (a baseline
    #            cannot referee its own edit).
    #
    # The selftest renders its fixture FROM the committed baselines, so an empty or broken template
    # cannot make it pass vacuously -- it asserts the render is non-vacuous and that every template
    # line round-trips render()->normalize() under two different fill values.
    check(
        "fork F3A arm-order baselines are well-formed (check_arm_order --check-baselines)",
        [sys.executable, os.path.join(REPO, "tools", "check_arm_order.py"), "--check-baselines"],
    )
    check(
        "fork F3A arm-order gate -- reorder/insert/delete go RED, a masked field does not",
        [sys.executable, os.path.join(REPO, "tools", "check_arm_order.py"), "--selftest"],
    )
    # F4F. The COMPLEMENT of the two rows above, not a duplicate of them. The arm-order gate proves
    # a module's sink is wired by watching its lines arrive in a real boot -- which is the stronger
    # evidence, and which it can only give for the four modules whose lines fall inside the arm
    # window. `mh::save` and `mh::ui` emit at save/load and lobby time, so no arm-order template can
    # carry them: delete either wiring and every gate in the tree still passes while the module goes
    # silent. This row is that gap, and it also pins the F4 fact the order gate cannot see -- WHICH
    # IMAGE defines each sink, checked against the committed libmh contract.
    check(
        "fork F4F module log sinks stay wired to seam_log and cross as the contract says",
        [sys.executable, os.path.join(REPO, "tools", "check_instrument_wiring.py")],
    )
    check(
        "fork F4F wiring gate -- a deleted/duplicated/misdirected wiring and a stale contract row go RED",
        [sys.executable, os.path.join(REPO, "tools", "check_instrument_wiring.py"), "--selftest"],
    )
    # mp:SES2. The THIRD leg of the instrument story, and it is about CONTENT where the two rows above
    # are about wiring and tools/data/arm_order/*.json is about ORDER. check_instrument_wiring pins
    # WHICH IMAGE writes each channel; check_arm_order pins WHICH STEP REPORTED WHEN; neither has any
    # opinion about what a line MEANS or who depends on it -- so a parser and an emitter could drift
    # apart with every gate in the tree green, and two of them had:
    #   * mp_pacing_report.read_frametimes still keys on a `qpc_freq=` header token D22 removed, so
    #     every frame-time column in the pacing report has been silently nan/0 on every current log;
    #   * mp_analyze's unplanned-end marker list still carries "kicked-off the game", which nothing in
    #     src/mh_dll emits.
    # Both were found BY building the registry, which is the argument for having one. The gate has
    # three arms (stale entry / unregistered parse / stale emitter) and the lint's docstring states
    # the matching rule and -- as load-bearingly -- its limit: arm B discovers only the `; [tag]`
    # class, because that is the one with a syntactic marker, not because it is the whole population.
    check(
        "log line formats are registered, and parser/emitter still agree (lint_log_formats)",
        [sys.executable, os.path.join(REPO, "tools", "lint_log_formats.py")],
    )
    check(
        "log-format gate -- a planted unregistered parse, a stale needle and a stale emitter go RED",
        [sys.executable, os.path.join(REPO, "tools", "lint_log_formats.py"), "--selftest"],
    )
    # tooling TL-GATE-D25FX. A hash-manifest change (D25 appended region 62) silently staled every
    # recorded `state` artifact -- the three libref world blobs + streams and both UI-REC oracles --
    # and the next full gate read them as "first mismatch at step 1", the shape of a broken replayer.
    # The rule "a manifest change means a re-capture IN THE SAME SESSION" is now a gate: the
    # fingerprint the DLL is built with (computed from the generated header, verified against the
    # DLL's own stamp on the blobs) must match every fixture's and oracle's stamp, and every declared
    # A/B/C excusal (tools/data/abc_excusals.json) must name a region the manifest still has, with
    # PROVED byte-level evidence beside it. Offline, sub-second.
    check(
        "recorded hash fixtures + UI-REC oracles are CURRENT, A/B/C excusals real (lint_fixture_currency)",
        [sys.executable, os.path.join(REPO, "tools", "lint_fixture_currency.py")],
    )
    check(
        "fixture-currency gate -- a stale fixture, an unstamped/stale oracle and a bad excusal go RED",
        [sys.executable, os.path.join(REPO, "tools", "lint_fixture_currency.py"), "--selftest"],
    )
    # tooling TL-GATE8. The code that decides which bytes reach the state hash (the watched files /
    # spans in tools/data/hash_input_epoch.json) may not change without a HASH_INPUT_EPOCH decision.
    check(
        "hashed-input code changes carry a HASH_INPUT_EPOCH decision (lint_hash_epoch)",
        [sys.executable, os.path.join(REPO, "tools", "lint_hash_epoch.py")],
    )
    check(
        "hash-epoch gate -- a watched edit, an unrebaselined bump and a bad span go RED",
        [sys.executable, os.path.join(REPO, "tools", "lint_hash_epoch.py"), "--selftest"],
    )
    # dist RP4. Structural checks over every tracked docker-compose file: the relay service runs
    # with `network_mode: host` (plan D4 -- the default bridge's userland proxy rewrites the
    # source address the connection-id demux depends on), the collector service does not, and
    # nothing references Watchtower (archived Dec 2025, not used -- images are pulled explicitly
    # by .github/workflows/deploy.yml). Pure YAML parse, no Docker, no rig, sub-second.
    check(
        "compose files (lint_compose)",
        [sys.executable, os.path.join(REPO, "tools", "lint_compose.py")],
    )
    check(
        "compose files -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_compose.py"), "--selftest"],
    )
    # SES1b. The three-match session-rollover scenario's post_check (tools/test_ui.py's
    # session_rollover entry) -- offline, planted-lane proof that the checker itself still catches
    # a wrong session-directory count, a mismatched/repeated match_id, a directory-name/match_id
    # mismatch, and (the negative arm this row exists for) a round whose session.json was never
    # closed, which is the shape a missing close-on-leave seam call leaves behind.
    check(
        "SES1b session-rollover post-check -- the negative cases still fire (check_session_rollover --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_session_rollover.py"), "--selftest"],
    )
    # mp:L1. The player-visible connection indicator's post_check (tools/test_ui.py's net_hud entry)
    # only runs when the rig does, so its own negatives are gated here off planted logs: an indicator
    # that armed but never reached a drawn frame, one whose link was never measured, a command-latency
    # number that has stopped being lookahead+step, a bar outside its own range, and an anonymous
    # stall -- the very thing the item replaced -- must each go RED rather than read as a quiet pass.
    check(
        "mp:L1 net-indicator post-check -- the negative cases still fire (check_net_indicator --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_net_indicator.py"), "--selftest"],
    )
    # tooling:TL-SHIMUDP-C. The udp-shim srtt post_check (tools/test_ui.py's shim_udp entry) only
    # runs when the rig does, so its own negatives are gated here off planted mh_lockstep.log
    # corpora: a bypassed shim (srtt reads near-zero, the LAN's real round trip), a transport that
    # measured nothing at all (every srtt0_ms is n/a), and a menu-session log with no rows must each
    # go RED rather than read as a quiet pass.
    check(
        "tooling:TL-SHIMUDP-C shim-rtt post-check -- the negative cases still fire (check_shim_rtt --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_shim_rtt.py"), "--selftest"],
    )
    # mp:R3a. The relay-path post_check (tools/test_ui.py's relay_punch entry) only runs when the
    # rig does, so its own negatives are gated here off planted mh_net.log corpora: a lane pinned to
    # force_relay (never reaches "udp path DIRECT") and a log with no path-switch line at all must
    # each go RED rather than read as a quiet pass.
    check(
        "mp:R3a relay-path post-check -- the negative cases still fire (check_relay_path --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_relay_path.py"), "--selftest"],
    )
    # mp:F3c. The codepage post_check (tools/test_ui.py's codepage_adopt / codepage_refused entries)
    # runs only with the rig; its negatives -- a joiner that never adopted, a host that refused
    # anyway, a refusal the client was never told of, a client that stayed seated (no join_refused
    # close), a host that admitted a peer it should have refused -- are gated here off planted logs.
    check(
        "mp:F3c codepage post-check -- the negative cases still fire (check_codepage_adopt --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_codepage_adopt.py"), "--selftest"],
    )
    # mp:RM1. The rematch-residue post_check (tools/test_ui.py's rematch_play entry) runs only with
    # the rig; its negatives -- a host game 2 entering with the previous match's residue + a step-50
    # desync, a joiner that never entered lockstep (p54bc=0, no sample), first samples that disagree
    # across the peers, a detector that sampled nothing (no game-1 rollup), a lane that never reached
    # the rematch -- are gated here off planted lanes.
    check(
        "mp:RM1 rematch-residue post-check -- the negative cases still fire (check_rematch_residue --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_rematch_residue.py"), "--selftest"],
    )
    # mp:SES7b. The match-replay tool's verdict and input resolution over planted logs and folders --
    # no rig: an excluded-region-only difference must not read as a divergence, too few common steps
    # must be REFUSED rather than IDENTICAL, and a process recording whose first played match is not
    # this one (its seed is process step 1) must be refused by name.
    check(
        "mp:SES7b match replay -- the negative cases still fire (replay_match_segment --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "replay_match_segment.py"), "--selftest"],
    )
    # mp:CH1. The cheat-gate post_check (tools/test_ui.py's ch1_cheat entry) runs only with the rig;
    # its negatives -- the cheat RAN (gate off), the refusal line missing, order 0xfa staged in a
    # peer's order buffers, a sim-path elimination, a desync sample, a netind name morph, nobody
    # submitted the line, a lane without a session or a harness log -- are gated here off planted lanes.
    check(
        "mp:CH1 cheat-gate post-check -- the negative cases still fire (check_cheat_gate --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_cheat_gate.py"), "--selftest"],
    )
    # mp:GS1(b). The ghost-slot post_check (tools/test_ui.py's ghost_exit_rejoin entry) runs only
    # with the rig; its negatives -- a committed horizon still pinned at the initial 10000 ms
    # advertisement on either peer, the REMOTE peer's printed slot frozen exactly at 10000 once the
    # match has run well past it (a side's own slot is never written by that side and sits there in
    # every healthy match too, so it is excluded), a vacuous/missing lockstep log, a process that
    # never launched a match at all -- are gated here off planted mh_lockstep.log fixtures.
    check(
        "mp:GS1(b) ghost-slot post-check -- the negative cases still fire (check_ghost_slot --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_ghost_slot.py"), "--selftest"],
    )
    # mp:GS2. The data-timeout post_check (tools/test_ui.py's gs2_data_timeout entry) runs only with
    # the rig; its negatives -- nobody drops (the pre-fix hang), both peers drop each other, the
    # frozen peer wrongly drops the healthy survivor, the watchdog fires early or late against its
    # own T, a --timeout-ms mismatch -- are gated here off planted lanes.
    check(
        "mp:GS2 data-timeout post-check -- the negative cases still fire (check_data_timeout --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_data_timeout.py"), "--selftest"],
    )
    # mp:U39. The negative-arm post_check (tools/test_ui.py's u39_diplomacy_echo entry) runs only
    # with the rig; its negatives -- no divergence at all, a real desync instead of the 4-step echo
    # window, the wrong region, the arm that NOPed after all, a divergence that never heals -- are
    # gated here off planted harness logs.
    check(
        "mp:U39 diplomacy-echo negative arm -- the negative cases still fire (check_u39_echo --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_u39_echo.py"), "--selftest"],
    )
    # mp:P6. The loser-gap post_check (tools/test_ui.py's p6_loser_gap entry) runs only with the
    # rig; its negatives -- a real freeze over the bound passing anyway, the same outcome on both
    # peers (the conquest workload never split winner/loser), a missing on_gameover line, baseline
    # mode wrongly asserting -- are gated here off planted mh_net.log/mh_frametime.log fixtures.
    check(
        "mp:P6 loser-gap post-check -- the negative cases still fire (check_p6_gap --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_p6_gap.py"), "--selftest"],
    )
    # mp:D28. The cancel-task post_check (tools/test_ui.py's d28_canceltask_local entry) runs only
    # with the rig; its negatives -- the seam banner missing, the Yes click never
    # routed, the routed arm still diverging, the reproduction arm identical or healing, the wrong
    # region, routing on in the reproduction arm -- are gated here off planted logs.
    check(
        "mp:D28 cancel-task post-check -- the negative cases still fire (check_cancel_task --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_cancel_task.py"), "--selftest"],
    )
    # mp:D35. The configuration-(1) build-click rows run only with the rig; their clauses -- the
    # seam armed/kept banner, the per-click probe line, hash identical/diverged-and-stayed in
    # pK_ai_econ, and the in-band desync watch clean/fired -- are gated here off planted logs.
    check(
        "mp:D35 build-click post-check -- the negative cases still fire (check_build_probe --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_build_probe.py"), "--selftest"],
    )
    # tooling:TL-SUITE-SPLICE-HOSTCLICK. host_clicks' three per-segment verdicts run only with the
    # rig; each segment going red ALONE (and an inherited divergence reading NOT JUDGED) is gated here.
    check(
        "host_clicks per-segment post-check -- each segment reds alone (check_host_clicks --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_host_clicks.py"), "--selftest"],
    )
    # mp:P9. The resync-storm post_check (tools/test_ui.py's match_launch_net / resync_storm_repro
    # entries) runs only with the rig; its negatives -- a configuration-(2) lane (libmh bound), an
    # UNCARRIED FIX line, a missing or MISMATCHED gate-install line, a FIRED line with the gate
    # carried, a too-short match, a desync, the verbose fragment lost, the storm arm with the gate
    # still on or too few fires -- are gated here off planted logs.
    check(
        "mp:P9 resync-storm post-check -- the negative cases still fire (check_resync_storm --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_resync_storm.py"), "--selftest"],
    )
    # tooling:TL-SUITE-FOLD-DETC1 / dist:V022. The orders-agree post_check (match_launch_net's third
    # post_check entry, folded from run_gate.py's retired det_c1 unit) runs only with the rig; its
    # negatives -- a missing mh_orders.bin, an empty recording, a common-step disagreement -- are
    # gated here off planted files, including the process/session-dir resolution both call shapes need.
    check(
        "dist:V022 orders-agree post-check -- the negative cases still fire (check_orders_agree --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_orders_agree.py"), "--selftest"],
    )
    # mp:P13. The sim-rate post_check (tools/test_ui.py's p13_rate_180 entry) runs only with the
    # rig; its negatives -- a slow whole match, a stall that only the worst-30-s window sees, a
    # mode-8 barrier row, a match too short for the window, a missing log -- are gated here.
    check(
        "mp:P13 sim-rate post-check -- the negative cases still fire (check_sim_rate --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_sim_rate.py"), "--selftest"],
    )
    # mp:CH1 (the AV clause). The no-crash-marker post_check (tools/test_ui.py's gs2_quit_frozen
    # entry) runs only with the rig; its negatives -- a marker from either peer during the run, a
    # stale marker from an earlier run wrongly attributed, a run dir with no game evidence -- are
    # gated here off planted lanes.
    check(
        "mp:CH1 no-crash-marker post-check -- the negative cases still fire (check_no_crash_marker --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_no_crash_marker.py"), "--selftest"],
    )
    # mp:SES6. Reads ONE peer's mh_net.log for the "net: udp counters" line's per-peer segments
    # (data_rx_age climbing, data_tx_age flat, horizon_ms frozen -- the outbound-delivery proof the
    # gs2_data_timeout scenario's shape already produces) and runs only with the rig; its negatives --
    # a horizon still moving (a slow link, not a stalled sim), both directions dead, rx_age that
    # dropped (data recovered), a single sample, a -1 horizon (nothing ever pushed), no counters line
    # at all -- are gated here off planted logs.
    check(
        "mp:SES6 outbound-delivery post-check -- the negative cases still fire (check_ses6_delivery --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_ses6_delivery.py"), "--selftest"],
    )
    # mp:EL2. The mother-deploy post_check (tools/test_ui.py's el2_mother_deploy entry) runs only
    # with the rig; its negatives -- either landing missing on either peer or at different game
    # clocks, the joiner landing before the host, a desync flagged or no agreeing sample past the
    # joiner's landing, the quit line missing / with the deploy's caller / with retail flags, no
    # outcome-4 dialog or U17 broadcast after it, the wrong session reasons, a survivor ended by
    # the transport-death fast-drop -- are gated here off planted logs.
    check(
        "mp:EL2 mother-deploy post-check -- the negative cases still fire (check_el2_mother --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_el2_mother.py"), "--selftest"],
    )
    # mp:L1b. The lobby slot-row ping column's post_check (wired into tools/test_ui.py's
    # match_launch_net entry) only runs when the rig does, so its own negatives are gated here off
    # planted `; [lobbyping]` corpora: the tick never writing a line at all, and every written line
    # permanently `n/a` (measured=0) -- must each go RED rather than read as a quiet pass.
    check(
        "mp:L1b lobby-ping post-check -- the negative cases still fire (check_lobby_ping --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_lobby_ping.py"), "--selftest"],
    )
    # mp:P9W. The receiver-deadline post_check only runs when the rig does, so its own negatives are
    # gated here off planted `; [resync] receiver_deadline:` corpora: no line at all, a barrier that
    # resolved normally (a RESUME landed, END with no deadline line), an elapsed_ms under the 2002 ms
    # floor (broken deadline math), and side_id=-1 (leader not found) -- each must go RED, and
    # `--expect absent` (the reproduction arm) must invert every one of those verdicts.
    check(
        "mp:P9W resync receiver-deadline post-check -- the negative cases still fire (check_resync_receiver_deadline --selftest)",
        [
            sys.executable,
            os.path.join(REPO, "tools", "check_resync_receiver_deadline.py"),
            "--selftest",
        ],
    )
    # mp:GX1. The overlay-residue post_check (tools/test_ui.py's gx1_overlay_residue entry) only
    # runs when the rig does, so its own negatives are gated here off synthetic BMP pairs: a planted
    # no-stamp arm (the AFTER capture identical to BEFORE) must go RED; a BEFORE capture with no
    # glyph at all, and a missing capture file, must each refuse rather than vacuously pass; and a
    # bright but WRONG-coloured AFTER (live terrain, not the overlay's own text colour -- the false
    # positive a plain luminance/"ink" threshold gave on the real rig: live strategic terrain
    # crosses the same brightness bar as the overlay's yellow text, so the threshold reddened
    # identically with the fix in and out -- it must match the CONFIGURED colour, with tolerance)
    # must NOT read as residue.
    check(
        "mp:GX1 overlay-residue post-check -- the negative cases still fire (check_overlay_residue --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_overlay_residue.py"), "--selftest"],
    )
    # mp:R4b. The relay-restart post_check (tools/test_ui.py's relay_restart entry) likewise runs
    # only with the rig; its negatives -- a peer that never got NOT_REGISTERED, a LOST with no
    # RESTORED, a restored leg whose link dropped anyway, a relay log with one `listening` line --
    # are gated here off planted corpora.
    check(
        "mp:R4b relay-restart post-check -- the negative cases still fire (check_relay_restart --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_relay_restart.py"), "--selftest"],
    )
    # mp:R6. The relay-rooms post_check (relay_match / relay_browse) runs only with the rig; its
    # negatives -- a host whose room IS its port, a host that never minted (a pre-R6 build), two
    # hosts in one room, a client that only ever came up in the directory room, a relay log with a
    # room_busy or a non-zero register_refused -- are gated here off planted corpora.
    check(
        "mp:R6 relay-rooms post-check -- the negative cases still fire (check_relay_rooms --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_relay_rooms.py"), "--selftest"],
    )
    # mp:R2b. The browser-rows post_check (browser_two_rows) runs only with the rig; its negatives --
    # one row listed where two were hosted, a join that dialled the FIRST lobby's room (the pre-R2b
    # shape), a join into a never-listed room, no `R2b join` line at all, one join where two were
    # expected, a listed room no host minted -- are gated here off planted logs.
    check(
        "mp:R2b browser-rows post-check -- the negative cases still fire (check_browser_rows --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_browser_rows.py"), "--selftest"],
    )
    # mp:R7a. The two dial-mode post-checks are mirror images: check_direct_dial fails when the client
    # contacted the relay on an *Internet server* dial (direct_dial_with_relay_set), check_relay_leg
    # fails when a first-browser join did NOT go through the relay (relay_browse_local). Both run only
    # when the rig does, so their own negatives are gated here off planted logs.
    check(
        "mp:R7a direct-dial post-check -- the negative cases still fire (check_direct_dial --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_direct_dial.py"), "--selftest"],
    )
    check(
        "mp:R7a relay-leg post-check -- the negative cases still fire (check_relay_leg --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_relay_leg.py"), "--selftest"],
    )
    # mp:R4a. The stale-relay post-check (relay_stale_notice) runs only with the rig; its negatives --
    # the R4a line missing, the notice never reaching mh.dll's carrier, a relay that counted no
    # mismatch, and a `relay protocol` line on a pair that should match -- are gated here off planted
    # logs.
    check(
        "mp:R4a stale-relay post-check -- the negative cases still fire (check_relay_stale --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_relay_stale.py"), "--selftest"],
    )
    # mp:SES3/SES3c (tooling:TL-SUITE-SPLICE-CAM). The camera-latch reader is the cam_latch
    # scenario's post-check, i.e. it only runs when the rig does -- so its own negatives are gated
    # here instead, off planted logs: a latch with no camera movement, a latch that never falls, a
    # run over the lines/frame budget, a run with mouse_trace off, and a --segment verdict confused
    # by the other probe's samples must each go RED rather than read as a quiet pass.
    check(
        "cam-trace reader -- planted latch/cost/absence cases go RED (check_cam_trace --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_cam_trace.py"), "--selftest"],
    )
    # mp:U19. The clean-quit reader is the graceful_quit scenario's post-check, so like the two rows
    # above it only runs when the rig does; its negatives are gated here off planted log pairs. The
    # arm that earns the row is "the relink ran before the broadcast": that was the real defect U19
    # found, every other clause stayed GREEN throughout it, and a reader that stopped noticing the
    # ordering would hand back a pass for a departure announcement that closed its own socket.
    check(
        "clean-quit reader -- planted relink-ordering/B2/slow-drop cases go RED (check_graceful_quit --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_graceful_quit.py"), "--selftest"],
    )
    # mp:U19f. The transport-death reader is the txdeath_ingame scenario's post_check, so like the
    # rows above it only runs when the rig does; its negatives -- no fast-drop line, no outcome-7
    # on_gameover, the U19e garbled-stream arm instead of the fast-drop route, and U19d's correction
    # firing on a REAL transport death (the wording must stay honest) -- are gated here off planted
    # logs, including the 2-peer AND (one bad peer fails the pair).
    check(
        "transport-death reader -- planted fast-drop/outcome/wording cases go RED (check_transport_death --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_transport_death.py"), "--selftest"],
    )
    # mp:U19b. The 3-peer clean-quit reader is the `--u19b-quit3` determinism shape's post_check
    # (run_u19b_quit3 in tools/test_ui.py), so like the row above it only runs when the rig does;
    # its negatives -- no quitter broadcast, a survivor's match ending (the clause's own failure
    # mode), a survivor learning it via the B2 fast-drop catch instead of the lockstep dispatch, a
    # non-clean mp_analyze verdict, and too few overlapping hashed steps compared (a vacuous-looking
    # green) -- are gated here off planted logs and a faked mp_analyze subprocess.
    check(
        "3-peer clean-quit reader -- planted broadcast/gameover/determinism cases go RED (check_quit_survivors_3peer --selftest)",
        [
            sys.executable,
            os.path.join(REPO, "tools", "check_quit_survivors_3peer.py"),
            "--selftest",
        ],
    )
    # mp:U19j -- the --u19j-gpfg3 shape's reader: the carrier FIRED on the leader after a gone
    # peer's frame, or (unguarded twin) the run went red on exactly that garbled frame.
    check(
        "gone-peer frame guard reader -- planted not-reached/no-FIRED/XPASS cases go RED (check_gone_peer_guard --selftest)",
        [
            sys.executable,
            os.path.join(REPO, "tools", "check_gone_peer_guard.py"),
            "--selftest",
        ],
    )
    # mp:X2, the same shape one item along. The map-download reader's verdict is a statement about
    # TWO peers' logs -- the host's claim and gate, the joiner's store and its own file before and
    # after -- and three of its clauses are ABSENCES (nothing transferred, nothing overwritten,
    # nothing written where the picker would list it). An absence-checker that had quietly stopped
    # matching would hand back a pass for every one of them, so the row that earns its keep is the
    # planted-negative sweep: a download under the base name, a download beside the player's maps,
    # an own-file hash that moved, a gate that never closed, and a transfer armed to a peer that
    # already had the map must each go RED.
    check(
        "map-download reader -- planted base-name/beside-maps/own-file-changed/no-gate cases go RED "
        "(check_map_transfer --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_map_transfer.py"), "--selftest"],
    )
    # mp:X2a -- the open-redirect reader: on the two independent rig VMs the client's OWN map
    # genuinely differs, so the `resolve stored ... -- redirecting ... to it` line is the one clause
    # that would go missing if the redirect were broken (on a local lane the base file IS the host's
    # content, so check_map_transfer's other clauses would still pass). A missing redirect line and a
    # redirect naming a path outside `mh_dl\` must each go RED.
    check(
        "map open-redirect reader -- planted no-redirect/outside-mh_dl cases go RED "
        "(check_map_redirect --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_map_redirect.py"), "--selftest"],
    )
    # mp:U41b. The two-match rollup reader: one match only, an inherited (equal) high-water, a
    # periodic rollup with no boundary line, and a carried non-zero counter must each go RED.
    check(
        "queue-rollup reader -- planted one-match/inherited/periodic/carried cases go RED "
        "(check_queue_rollups --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_queue_rollups.py"), "--selftest"],
    )
    # F1F. The interior-pointer screen stays reproducible from committed inputs (fixture blob +
    # generated registry header), and the TLO_REGISTRY sole-reader claim is a gate, not prose --
    # the world blob imports that region's image pointers VERBATIM, and only materialise_tlo()'s
    # rebased-bind refusal keeps them from ever being dereferenced. Each self-check carries its
    # own positive control (the blob's own census, the G_TEXT_PTRS entry-for-entry adjudication),
    # so a broken scan reads as red, never as a clean zero.
    check(
        "interior-pointer screen self-checks + TLO reader gate (scan_interior_ptrs --check)",
        [sys.executable, os.path.join(REPO, "tools", "scan_interior_ptrs.py"), "--check"],
    )
    # LIB-DISPATCH-SA. A region a sim body indexes as [row * STRIDE + i] must be longer than ONE
    # row. This is a DRIFT gate on the generated header; the assertions themselves are evaluated by
    # the COMPILER (the stride is in elements and the reach in bytes, so only it can compare them),
    # which is why the mutation proof is a build failure rather than a lint failure: shrink the
    # claim in the registry and mh/sim fails to compile, naming the region.
    #
    # The bug it exists for was invisible for five weeks -- _G_LLM_STRAT_MOVE_MICROSTEPS claimed one
    # of its 24 heading rows, and inside mh.exe the over-index lands in the game's own .bss where
    # the other 23 really are, so every read was CORRECT. Only a host that binds each region into
    # its own allocation can see it, which is what LIB-REF's standalone replay did.
    check(
        "strided regions are longer than one row (lint_strided_regions --check)",
        [sys.executable, os.path.join(REPO, "tools", "lint_strided_regions.py"), "--check"],
    )
    # SIM-BOUNDARY. The ORIGINAL-accessor census the interlock's rules 4-6 read: how many original
    # functions still write/read each region. Committed data (the check is Ghidra-free), so what can
    # rot is the region SET -- add a region to the registry and its accessors are unknown, which is
    # exactly the state a region must not be in when someone declares it moved. This gate refuses the
    # gap; gen_state_registry --check refuses the move.
    check(
        "region-accessor census is complete + current (gen_region_accessors --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_region_accessors.py"), "--check"],
    )
    # R3b (LIFT-R3B). Every notify scope's transitive write set, intersected with the regions
    # translated libmh READS. R3 covers a notify entry whose RETURN the sim consumes; R3b covers the
    # other half -- one that returns nothing but whose writes libmh reads back in the SAME FRAME.
    # That works today only because the hosted config dispatches synchronously at emit; a poll host
    # drains at frame edge and libmh would read the previous frame's value, which no oracle we own
    # would catch (the hosted arm is the only arm the wall is ever tested in).
    #
    # THE GATE EXISTS BECAUSE THE RULE ARRIVED AFTER THE CODE. R3b was written at LIFT-TACT's opening,
    # 49-71 minutes AFTER the on_screen scopes had already landed, and nothing ran it backwards over
    # them; the 2026-09-04 hand sweep was that missed check. This makes the check mechanical, so a
    # scope added tomorrow is measured the moment it is added rather than whenever someone remembers.
    # The intersection is a worklist, not a verdict -- it cannot see frame ORDER -- so
    # tools/data/notify_readback_dispositions.json answers each one and the gate refuses anything
    # unlisted, anything listed without a reason, and any exemption that has gone stale.
    check(
        "R3b notify readbacks all dispositioned (gen_notify_readback --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_notify_readback.py"), "--check"],
    )
    check(
        "R3b readback gate -- the negative cases still fire (gen_notify_readback --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "gen_notify_readback.py"), "--selftest"],
    )
    # SB2's currency guard classifies drift in two directions -- unsafe (a rename or a new region can
    # hide an accessor) vs conservative (more functions are ours than the census knows, which can only
    # over-report). Collapsing them would make the check either useless or unrunnable, so the
    # classification itself is armed.
    check(
        "census currency guard classifies drift (gen_region_accessors --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "gen_region_accessors.py"), "--selftest"],
    )
    # ---- SB-SOLE's WRITER-ATTRIBUTION ROWS DROPPED AT FORK F2E (ruling Q4) -----------------------
    #
    # Two rows stood here: every ORIGINAL writer of a region libmh writes has a terminal disposition
    # -- a MIGRATION-LEDGER ROW, or a hand-authored exception -- plus its selftest. The population and
    # the verdict both came out of tools/data/*_migration.json (states `verified`/`dead`), which makes
    # it a migration-ledger currency gate by Q4's definition however region-shaped it looks: what it
    # actually asks is "has this writer been migrated yet, and if not is there a reason on file".
    #
    # THE DATA AND THE TOOL BOTH STAY. tools/data/writer_dispositions.json is a hand-authored record
    # of 122 adjudications and gen_writer_attribution.py is still importable (gen_seam_surface and
    # migration_seeds both use it) and still runnable as a report. Only the GATE goes -- the F0
    # frozen-inputs shape, one drop later.
    # LIB-BOOT. libmh imports the cfg parser's OUTPUT instead of owning the 65 KB parser, and the
    # question that decides whether that import is honest is not whether a hash matches -- a hash
    # over three blocks matches too -- but whether every region the cfg load writes is either in the
    # snapshot or excluded for a stated reason. The population is DERIVED (the write closure of boot
    # stage 5's root) and the unaccounted count must be zero, so a region the closure newly matches
    # turns this red rather than silently falling out of the world libmh builds.
    check(
        "post-cfg snapshot schema: 0 unaccounted cfg-cluster regions (gen_boot_snapshot --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_boot_snapshot.py"), "--check"],
    )
    check(
        "boot-snapshot accounting refusals still fire (gen_boot_snapshot --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "gen_boot_snapshot.py"), "--selftest"],
    )
    # LIB-WORLD, the same shape one scope wider: the step-0 fixture LIB-REF replays against is a dump
    # of every BOUND region, so its population is the whole registry and its accounting question is
    # the same one -- carried, or excluded for a stated reason, with the unaccounted count zero. The
    # gate matters here for a reason the boot one does not have: the registry GROWS (it gained two
    # regions in this very item), and a region added without a re-derived schema would be a region
    # the fixture silently does not carry and the standalone replay silently reads unbound.
    check(
        "step-0 world schema: 0 unaccounted registry regions (gen_world_snapshot --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_world_snapshot.py"), "--check"],
    )
    check(
        "world-snapshot accounting refusals still fire (gen_world_snapshot --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "gen_world_snapshot.py"), "--selftest"],
    )
    # LIB0 (the endgame plan). Three gates, one direction of travel: the outward-call census is
    # the lib-complete burn-down number and must not go stale (a new mh::call:: site without a
    # regenerated census is exactly the drift the number exists to see); the libmh.vcxproj is
    # GENERATED from the module tree, so a module .cpp added without regenerating silently ships a
    # lib that misses it; and a direct harness include in module code must carry its MH_LIBMH_BUILD
    # guard or the lib build re-couples to the injection layer.
    check(
        "libmh outward-call census is current (gen_libmh_calls --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_libmh_calls.py"), "--check"],
    )
    check(
        "all three libmh projects match the module tree (gen_libmh_vcxproj --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_libmh_vcxproj.py"), "--check"],
    )
    # fork F5P: the .vcxproj.filters trees. A filters file is a SECOND copy of a project's item
    # list -- the copy Visual Studio rewrites when someone drags a file in Solution Explorer, and
    # the copy nothing else in this repo reads. Both halves are gated: the generated three ride the
    # row above (same tool, same pass), the nine hand projects ride this one. The folder tree is
    # derived from the on-disk layout, so "stale" here means the tree stopped matching the disk.
    check(
        "libmh project filters still fail on planted staleness (gen_libmh_vcxproj --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "gen_libmh_vcxproj.py"), "--selftest"],
    )
    check(
        "every hand project's .filters mirrors its layout (gen_vcxproj_filters --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_vcxproj_filters.py"), "--check"],
    )
    check(
        "the .filters drift rules still fire (gen_vcxproj_filters --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "gen_vcxproj_filters.py"), "--selftest"],
    )
    # fork F5I. The selftest gate's SUITE LIST, which existed three times with nothing comparing the
    # copies -- a tuple in run_selftests.py, the dispatch table in net_selftest.cpp, and that
    # dispatcher's hand-written mode message. A suite added to the exe and not to the tuple simply
    # never ran, and the gate stayed green while doing less. tools/data/selftest_roster.json is now
    # the committed statement of the table, and this is its no-build arm: it PARSES THE SOURCE,
    # because lint has no toolchain. run_selftests.py carries the stronger half -- the same
    # assertion against the exe's live `--list-suites` -- and neither subsumes the other.
    check(
        "the selftest roster matches the exe's suite table (check_selftest_roster --check)",
        [sys.executable, os.path.join(REPO, "tools", "check_selftest_roster.py"), "--check"],
    )
    check(
        "selftest-roster drift -- the negative cases still fire (check_selftest_roster --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_selftest_roster.py"), "--selftest"],
    )
    # fork F4G: the STANDALONE arm's export contract -- configuration (3)'s libmh.dll
    # (Release\standalone\) and the list its host imports. Same two-half split as F4D's one row down:
    # this half re-renders libmh_std.def from the committed list and needs no build, and
    # `--rederive --check` re-measures from libref_host's and the archive's objects in run_gate.
    check(
        "the standalone libmh export contract is current (gen_libmh_std_contract --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_libmh_std_contract.py"), "--check"],
    )
    check(
        "standalone libmh contract rules -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "gen_libmh_std_contract.py"), "--selftest"],
    )
    # fork F4G: the config-(3) gate unit's own judgments -- the asset declaration's falsifier and the
    # short-run rule. The REPLAY needs the Release build and 70 s of CPU, so it is a run_gate unit
    # (`libref`); these are the parts of it that are decidable in a second without one.
    check(
        "config (3) replay rules -- the negative cases still fire (replay_libref --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "replay_libref.py"), "--selftest"],
    )
    # fork F4D. The SOURCE-ONLY half of the export contract: the committed row list is the authority
    # here and the three emitted files must re-render from it byte for byte. It needs no build, which
    # is why it can be a lint row at all -- the other half (does the committed list still match what
    # the two images' objects actually say?) is `--rederive --check`, which run_gate runs straight
    # after the Release build, in the same place --subset runs and for the same reason.
    check(
        "the libmh export contract's emitted files are current (gen_libmh_contract --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_libmh_contract.py"), "--check"],
    )
    check(
        "libmh contract rules -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "gen_libmh_contract.py"), "--selftest"],
    )
    # fork F4E, and the same split for the same reason one module over: the harness's two IMPORT
    # contracts (27 host rows out of mh.dll, 30 spine rows out of libmh.dll) plus its own 12-row
    # export list. This half re-renders the four emitted files from the committed row list and needs
    # no build; `--rederive --check` re-measures from the three images' objects and runs in run_gate.
    check(
        "the harness contracts' emitted files are current (gen_harness_contract --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_harness_contract.py"), "--check"],
    )
    check(
        "harness contract rules -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "gen_harness_contract.py"), "--selftest"],
    )
    # LIB-ABI: the host-callback table is generated from the adjudication ledger's
    # host-callback:* rows -- a reclassed row or a changed prototype without a regenerated
    # table would ship a host ABI that silently disagrees with the adjudication.
    check(
        "host-callback table matches the ledger (gen_libmh_hostapi --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_libmh_hostapi.py"), "--check"],
    )
    # The per-entry caller evidence must describe the CURRENT tables -- two 2026-09-10 slices
    # shrank the table without regenerating it, and the stale evidence read as authoritative.
    check(
        "host-callback caller evidence is current (gen_hostapi_callers --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_hostapi_callers.py"), "--check"],
    )
    # LIB-REBIND: the rebind binder is derived from the seams + the manifests, so a new seam, a
    # renamed wrapper, or a translated row entering the binder set changes it -- and a stale binder
    # is not a compile error, it is a row that silently keeps binding the ORIGINAL.
    check(
        "rebind binder matches the seams and manifests (gen_libmh_rebind --check)",
        [sys.executable, os.path.join(REPO, "tools", "gen_libmh_rebind.py"), "--check"],
    )
    check(
        "no libmh module names the host-only stock-base table (lint_libmh_layering)",
        [sys.executable, os.path.join(REPO, "tools", "lint_libmh_layering.py")],
    )
    check(
        "host-only-table refusals still fire (lint_libmh_layering --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "lint_libmh_layering.py"), "--selftest"],
    )
    # F4D-PRE. The STRONGER statement of the same subject, and the two rows are siblings rather
    # than duplicates: lint_libmh_layering asks "is every harness include GUARDED", this asks "is
    # there a harness include AT ALL, guarded or not" -- plus the token scan and (outside lint) the
    # object pass. The guarded form was the arrangement eight roster TUs used until this item, and
    # it hides the edge from a link-level reader while leaving it in the HOSTED build, which is the
    # configuration F4D ships.
    #
    # --src-only HERE, and the reason is the same division check_net_lockstep_refs draws: the OBJ
    # mechanism needs a Debug build and lint does not build. That matters more for this tool than
    # for that one, because SRC is genuinely the weaker half here -- libmh's largest outbound row
    # arrived through a MACRO and no source scan could see it. So the item's own proof and any
    # session touching the roster run the full thing:
    #     python tools/check_libmh_outbound.py          (after a Debug build)
    check(
        "libmh outbound edge: the roster reaches its host only through the registration surface "
        "(check_libmh_outbound --src-only)",
        [sys.executable, os.path.join(REPO, "tools", "check_libmh_outbound.py"), "--src-only"],
    )
    check(
        "libmh outbound-edge refusals still fire (check_libmh_outbound --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "check_libmh_outbound.py"), "--selftest"],
    )
    # ---- THE CALL-OUT SEAM ROWS DROPPED AT FORK F5M S4b (the archive-tool cut) ------------------
    # They gated the seam surface of a MIGRATION ITEM: the callees that stay original under Law 4 and
    # are reached through the generated call table, each needing a disposition. Two findings are
    # worth keeping out of it. (1) The SIZE of a frontier was deliberately never the gate -- every
    # closed domain published one (sim 55 callees, orders_issue 26, ai 7) and none was ever blocked
    # by it; what had to be gated was the count of call-outs in a checked scope with NO disposition.
    # (2) The reachability walk it owned ("is this function reachable from its own transitive outward
    # callees") is invisible to a pairwise check scoped to the migration set, which cannot see a
    # cycle that leaves the set. Both are statements about PLANNING a migration item, the era the
    # fork closes -- and its comparison also read a research document the cut does not carry.
    # O4-ISSUE. Every entry of the order-issue surface has a NAMED owner, and every accessor of an
    # order region does too. Gated rather than reported because the surface is a QUERY: it gains
    # members when the binary is renamed or a seed widens, and a gained member arrives owned by
    # nobody. The generators print that as a membership delta, which fails nothing and is absorbed
    # into the next run's baseline -- so the attribution is what has to be the gate.
    # ---- THE MANIFEST CURRENCY GATE DROPPED AT FORK F2E (ruling Q4) -----------------------------
    #
    # `gen_migration --check-all` re-derived each domain's ledger and failed if the committed file
    # differed. It was added 2026-08-28 because a PRINTED membership delta fails nothing and the next
    # run absorbs it -- four of five manifests had drifted that day and one had silently invalidated a
    # `done` tracker item. That reasoning was right while the manifests were a moving record of a
    # migration in progress. The reimplementation era is closed and the ledgers are now history: a
    # currency gate over a file nothing updates asserts that nobody edited it, which git already says
    # more precisely. gen_migration.py itself STAYS -- gen_libmh_inbound and migration_seeds import it.
    # ---- THE ORDER-ISSUE ATTRIBUTION ROWS DROPPED AT FORK F5M S4b (the archive-tool cut) --------
    # They asserted that every entry of the order-issue surface, and every accessor of an order
    # region, had a NAMED owner -- gated rather than reported because the surface is a QUERY that
    # gains members when the binary is renamed or a seed widens, and a gained member arrives owned by
    # nobody while the generator only prints it as a membership delta the next run absorbs. The
    # --check half diffed against a committed research inventory the cut does not carry, so it was
    # already STALE-by-construction on a public tree -- the F5E regression the F5M measurement found;
    # the two entries excusing it in the --ci skip table leave with these rows.
    # ---- THE TACTICAL-MEMBERSHIP ROW DROPPED AT FORK F5M S4b (the archive-tool cut) -------------
    # It kept the EXCLUSIVELY-tactical closure current -- the call-graph answer to "which functions
    # belong to tactical mode", as opposed to the `llm_tact_` prefix count every earlier estimate
    # used. The measured finding is the reason the distinction mattered and is worth keeping: 11
    # prefixed functions are NOT members and 28 unprefixed ones ARE, so the two answers are
    # genuinely different sets and a prefix count was never a membership answer. The closure itself
    # is a migration-planning artifact derived from a Ghidra call-graph dump.
    # ---- THE ORDER-MATRIX ROWS DROPPED AT FORK F5M S4b (the archive-tool cut) -------------------
    # They gated the order issue-dispatch coverage matrix: --check was the ordinary drift gate (both
    # inputs are name-keyed, so a rename silently ages it) and --self-check pinned the one step of
    # that pipeline that is REASONED rather than read -- the routing arithmetic `case = entries-1-j`
    # recovered from the REPNE SCASB scan -- against three mappings derived by hand before the tool
    # existed, and it was verified to go red on a one-off in that expression. Like the inventory
    # above, --check's right-hand side is a committed research document the cut does not carry.
    # ---- THE MIGRATION-LEDGER SCHEMA ROW DROPPED AT FORK F5M S4b (the archive-tool cut) ---------
    # It enforced a CLOSED vocabulary for the ledgers' `state` and `evidence_tier` fields and the
    # tier<=>verified invariant, so a `reviewed` row could not silently accumulate a tier. The
    # measured failure behind it: the sim ledger had drifted into 37 such rows, disagreeing with the
    # session driver's own done metric and with the tracker. The vocabulary is still enforced where
    # it can still be breached -- the manifest generator validates the merged rows against that same
    # schema module before it writes, which is the only moment a new row is minted.
    # ---- THE PROMOTION-RECONCILIATION AND POPULATION ROWS DROPPED AT FORK F2E -----------------
    #
    # Six rows stood here: the ledgers' `promoted` flag against the tree, the whole promotion
    # reconciliation (section 5's "no owned body is still reachable as an original"), the
    # pruned-closure-root refusal, the undefined-code CALL ratchet, and the tact population closure
    # with its selftest. Every one of them measured MIGRATION PROGRESS -- how much of a domain had
    # changed hands, and whether the ledgers still described the tree honestly while that number
    # moved. Fork ruling Q4: the reimplementation era is closed, so the ledgers freeze as history
    # and a currency gate over a frozen file can only ever be green or broken.
    #
    # WHAT DID NOT DROP, because it is a different question: the ledger SCHEMA check above (a closed
    # vocabulary is a property of the file, not of the migration's progress) and the tracker<->ledger
    # agreement below. The 349-file proof suite is untouched -- it proves the bodies, not the flags.
    # tools/report_promotion_reconciliation.py and tools/report_migration_counts.py are deleted;  # CITATION-OK
    # their committed inputs (reconciliation_*.json, undef_call_dispositions.json) stay as history.
    # ---- THE TRACKER<->LEDGER AGREEMENT ROW DROPPED AT FORK F5M S4b (the archive-tool cut) ------
    # The other half of the same guard: an item marked status=done whose ledger batch still held
    # unfinished rows failed outright, and one holding review-only rows had to SAY so, so that "done"
    # could never quietly mean "and N of them were never proven" (the gap the batch-D question
    # exposed). Both sides of that comparison -- the tracker domain and the ledgers -- are private
    # research layers the cut does not carry, so the row leaves with them.
    # The rig's crash detector. Its failure mode is a FALSE NEGATIVE -- matching nothing and thereby
    # reporting "no crash" at the moment someone is deciding whether a stall is a hang -- so the lane
    # filter's negative cases are the point, not decoration. It caught a real prefix-matching bug in
    # itself on its first run (`ui_sp_det` claiming `ui_sp_det2`'s crash).
    # G99: the UI suite's shared-cause detector. It ABORTS the suite, so a false positive is as
    # expensive as a miss -- its selftest asserts BOTH directions against a real healthy log.
    check(
        "UI-suite install-refusal detector (test_ui --selftest-refusals)",
        [sys.executable, os.path.join(REPO, "tools", "test_ui.py"), "--selftest-refusals"],
    )
    check(
        "crash attribution (crash_report --selftest)",
        [sys.executable, os.path.join(REPO, "tools", "crash_report.py"), "--selftest"],
    )
    # 2026-07-31: the tracker YAML had silently accumulated 181 runs of mojibake (every em dash, the
    # middle dots, the box-drawing banners) and its generated view inherited all of it.
    # Cause: a UTF-8 file read with the LOCALE codepage (cp1251 here) and rewritten as UTF-8 -- one
    # layer of damage per pass, no exception raised. Reading and writing with the same wrong codec
    # round-trips clean, which is why it survives casual testing.
    check(
        "text encoding (no UTF-8 re-decoded through a legacy codepage)",
        [sys.executable, os.path.join(REPO, "tools", "lint_encoding.py")],
    )
    # C8's done_when (1): the direct-call rule as a machine, not a paragraph. Its breach is one token
    # and is INVISIBLE in the shipping config -- an entry-routed intra-closure call lands on our own
    # replace thunk and behaves identically. Nothing goes wrong until someone builds the entry-routed
    # arm, reads a seam liveness counter, or tries to bisect. Proven to go RED by deliberately
    # reverting one edge before it was committed, not asserted.
    # ---- THE SESSION-LOG INDEX ROWS DROPPED AT FORK F5M S4b (the archive-tool cut) --------------
    # They capped the per-day entries of the research repo's session-log index at 300 characters. The
    # finding is a general one and is kept here rather than lost with the row: the index was defined
    # as "one link per day with a one-line description", "one line" got read as "one PHYSICAL line",
    # and the section reached 87 KB -- 81% of its own file -- at a median of 1744 chars, worst a
    # single 20603-character line. An earlier split had already fixed the symptom once by moving the
    # diary out; it regrew because the prose simply migrated into the index lines and nothing
    # checked. Discipline alone failed twice; a hard lint held. The index it guards is part of the
    # private research layer, so both rows leave with it (and with their --ci skip entries).
    check(
        "direct-call rule (lint_internal_edges)",
        [sys.executable, os.path.join(REPO, "tools", "lint_internal_edges.py")],
    )
    # No `live_*_calls()` inside a `detail::` body. Added 2026-08-31 after four sim_resid
    # translations shipped with one, which made three finished offline oracles unrunnable: the
    # table holds `mh::call::` naked thunks against absolute game VAs, so the case FAULTS before
    # its first assertion instead of failing. Nothing else could see it -- it compiles, links,
    # and satisfies lint_translation (which reads the same struct to check callee COVERAGE).
    # translator-brief 3b states the hazard and then exempts a SAME-TU sibling; these were
    # cross-TU, and the brief never said where a cross-TU sibling's table comes from.
    check(
        "no live_*_calls() in a detail body (lint_detail_calls)",
        [sys.executable, os.path.join(REPO, "tools", "lint_detail_calls.py")],
    )
    check(
        "detail-call rule -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_detail_calls.py"), "--selftest"],
    )
    # A PROMOTED body may not drop a return something reads. Unpromoted, a wrong `void` prototype is
    # latent -- the original's own epilogue still runs; promoted, our C++ returns whatever was left
    # in EAX. That shipped once (llm_tutorial_step_driver, EN v397): a real `return 0` typed void,
    # and its caller is a callback pump that UNINSTALLS anything returning nonzero, so the tutorial
    # ran one tick. Nothing offline could see it -- the body's own suite called it and discarded the
    # result. Data comes from tools/dump_epilogue_returns.py (Ghidra-side), so this stays fast.
    check(
        "no promoted body drops a consumed return (lint_dropped_returns)",
        [sys.executable, os.path.join(REPO, "tools", "lint_dropped_returns.py")],
    )
    check(
        "dropped-return gate -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_dropped_returns.py"), "--selftest"],
    )
    # Session-diary narration in src/mh_dll comments is a RATCHET: unattended sessions read source
    # files and imitate them, so diary markers (slice ordinals, session ids) both propagate stale
    # framing and breed more of themselves (2026-08-26: tact_pilot.cpp hit 46% comment lines of
    # diary; the "blocked on TACT-CUT2" myth rode exactly this channel). Baseline holds the
    # pre-existing 851 lines; any growth per file fails.
    check(
        "no new session-diary narration in src/mh_dll (lint_source_narration)",
        [sys.executable, os.path.join(REPO, "tools", "lint_source_narration.py")],
    )
    check(
        "source-narration ratchet -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_source_narration.py"), "--selftest"],
    )
    check(
        "patch/seam interlock (lint_dll_patches)",
        [sys.executable, os.path.join(REPO, "tools", "lint_dll_patches.py")],
    )
    check(
        "patch/seam interlock -- negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_dll_patches.py"), "--selftest"],
    )
    # C7. All three rules are about a determinism run NOT meaning what it appears to mean, and every
    # one of them is SILENT when it is working -- exactly the shape that rots unnoticed between rig
    # runs. Rig-free, so it belongs in the fast gate rather than the 30-minute one.
    check(
        "determinism run-shape rules (det_arms --det-selftest)",
        [sys.executable, os.path.join(REPO, "tools", "det_arms.py"), "--det-selftest"],
    )
    # C9. Markdown tables that do not render. Found 2026-08-03 with 58% of the failure ledger -- the
    # ledger consulted before every "dead/unwired" claim -- rendering as prose, and four of its rows
    # serving WRONG instructions because an unescaped `|` in a code span is deleted from the rendered
    # text (`FLAGS |= 0x40` read as `FLAGS = 0x40`). Invisible in source, invisible in review, and it
    # accumulated for weeks. Read-only, sub-second, no Ghidra.
    check(
        "markdown tables render (lint_md_tables)",
        [sys.executable, os.path.join(REPO, "tools", "lint_md_tables.py")],
    )
    check(
        "markdown tables -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_md_tables.py"), "--selftest"],
    )
    # ---- THE LOOP-DRIVER GRACEFUL-STOP ROW DROPPED AT FORK F5M S4b (the archive-tool cut) -------
    # It gated the unattended migration loop's wind-up path. The reason it was worth a lint row is
    # general enough to keep: the wind-up MESSAGE is the only thing a two-hour-old session ever sees,
    # so it has to keep asking for exactly what postflight will fail it on, and an edit that softened
    # the "clean tree" line would cost a real run's work at the worst possible moment -- while
    # someone is already stopping it. The loop drives the private research layers and goes with them.
    # AI0. The AI state view's write rejection is a COMPILE-time property, so nothing else in this
    # gate can observe it: the DLL building proves the legal code compiles and says nothing about the
    # illegal code failing to. Six cl.exe invocations, ~8 s, no game and no rig -- and it belongs here
    # rather than in a script somebody remembers to run, because the failure mode is a `const` quietly
    # disappearing from a header during an unrelated edit and nothing ever noticing.
    # SIM0 added a second target to the same driver (the sim's mutable interface: W1 const view /
    # W2 no escaping address / W3 no forged store), so this one invocation now covers both modules.
    check(
        "state views reject illegal writes at COMPILE time (check_const_view: ai + sim)",
        [sys.executable, os.path.join(REPO, "tools", "check_const_view.py")],
    )
    # SIM0's other half, and the one the compiler cannot express: W2 says no game-state ADDRESS
    # reaches a sim translation, which is only true while sim_state.cpp is the single binder. A
    # `mh::state::ptr<>` or a literal VA in any other libmh/sim/ TU re-creates exactly the staleness
    # ST2's rebase exists to remove -- and it would compile, run, and pass every other check.
    # Sub-second, pure text, no toolchain.
    check(
        "no raw state address in a sim TU (check_sim_addresses)",
        [sys.executable, os.path.join(REPO, "tools", "check_sim_addresses.py")],
    )
    check(
        "sim raw-address check -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "check_sim_addresses.py"), "--selftest"],
    )
    # SB-HOSTFREE: the same discipline, asked of the WHOLE dll rather than the seven migrated
    # modules, and only about regions a relocating host may actually MOVE. Such a constant reads
    # 0xCD under `[harness] relocate_state=1` -- measured: harness.cpp printed `master_gate=
    # 3452816845` in its own AI banner and the run still said PASS. Ratcheted at the 16 that remain
    # in mh/seams; it also tightens by itself, since the movable set grows as promotion retires the
    # original accessors that pin regions in place.
    check(
        "no movable-region address spelled as a constant (check_movable_addresses)",
        [sys.executable, os.path.join(REPO, "tools", "check_movable_addresses.py")],
    )
    check(
        "movable-address ratchet -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "check_movable_addresses.py"), "--selftest"],
    )
    # SB-HOSTFREE: every save block must be servable under a relocated bind. Ten run past the symbol
    # they start at, and for four of them the overrun lands in OTHER LIVE REGIONS -- which under a
    # relocating host reads those regions from the copies they abandoned, silently, because
    # covering() resolves against `reach`. Those four are decomposed into per-region runs; this
    # refuses a new one that is not.
    check(
        "every save block is servable relocated (check_save_block_slices)",
        [sys.executable, os.path.join(REPO, "tools", "check_save_block_slices.py")],
    )
    check(
        "save-block slice check -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "check_save_block_slices.py"), "--selftest"],
    )
    # SB-BIND T2: no production index expression uses a compile-time roster cap. Scoped to the
    # module tree -- mh_nettest fixtures legitimately use the constants, and some declare real C
    # arrays with them. A single re-introduced site is worse than the pre-T2 state: it disagrees
    # with its neighbours about where a player's row starts, where uniform staleness at least
    # mis-indexed consistently.
    check(
        "no compile-time roster cap in module code (check_roster_caps)",
        [sys.executable, os.path.join(REPO, "tools", "check_roster_caps.py")],
    )
    check(
        "roster-cap check -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "check_roster_caps.py"), "--selftest"],
    )
    # SB-BIND T3: every region bound with a POINTER element type carries a recorded disposition.
    # Generated rather than remembered because a hand count recorded this shape as having
    # ONE instance and there are four -- each added without anyone re-reading the line.
    check(
        "pointer-valued region slots adjudicated (scan_pointer_slots)",
        [sys.executable, os.path.join(REPO, "tools", "scan_pointer_slots.py"), "--check"],
    )
    check(
        "pointer-slot adjudication -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "scan_pointer_slots.py"), "--selftest"],
    )
    # ---- THE READINESS READ-SET ROW DROPPED AT FORK F5M S4b (the archive-tool cut) --------------
    # It pinned the regexes of a TEXT SCAN over exported disassembly listings, for the reason any
    # scan-shaped check needs pinning: a regex that stops matching reports GREEN. Nothing else
    # exercised them, because the readiness report is deliberately not part of any gate -- it is a
    # SCHEDULING predicate, not a correctness one. Both the report and the listings are migration-era.
    # ---- THE SEED-QUERY REFUSAL ROW DROPPED AT FORK F5M S4b (the archive-tool cut) --------------
    # It proved the seeded-membership refusals fire. The finding to keep is the failure mode it was
    # built around, which generalises to every derived population in this tree: a membership QUERY
    # fails by returning an empty or silently narrowed set, AND AN EMPTY LEDGER READS EXACTLY LIKE A
    # FINISHED ONE. Seeded domain membership is migration-era machinery.
    # ---- THE STATE-INTERFACE OWNERSHIP ROWS DROPPED AT FORK F5M S4b (the archive-tool cut) ------
    # They asserted every migration domain had a tracker item owning the STATE INTERFACE its batches
    # translate into. The finding is the one worth carrying past the fork: A MISSING ITEM LEAVES NO
    # ROW TO BE WRONG. The tact domain was stood up with a profile, a manifest, an oracle and an
    # arming answer and had no such item -- its two open items said "translate nothing" and
    # "decompose nothing" -- so nobody owned the interface and the ledger looked complete. Nothing
    # else in the gate could see it; it surfaced only because a human asked which item covered a
    # phrase in a session report. Its subject is the tracker domain, which the cut does not carry.
    # ---- THE DOC-SYMBOL RESOLUTION ROWS DROPPED AT FORK F5M S4b (the archive-tool cut) ----------
    # They scanned the research docs for symbol names the Ghidra database no longer has. Two findings
    # are worth keeping. (1) The scale: the first run found 68 stale names across 18 files, including
    # `llm_unit_bldg_charge_add_scaled` for what is really `llm_unit_bldg_apply_scaled_damage` -- the
    # semantic OPPOSITE, in the order-dispatch table. Prose explaining a mechanism stops naming it,
    # silently, because the naming campaigns rename constantly. (2) Why it stayed ADVISORY (user,
    # 2026-08-05): a name-scanner cannot tell a stale reference from the docs' compact SET notation
    # -- it split `_G_LLM_STRAT_SUBTICK_A/B_PERIOD[_POS|_NEG]` at the slash and reported a symbol
    # nobody wrote -- and blocking on that pressures the next author to "fix" correct prose. The docs
    # it scans are the research record; both rows and the skip entry excusing them leave together.
    # ---- THE AGENT-WORKFLOW PARSE ROWS DROPPED AT FORK F5M S4b (the archive-tool cut) -----------
    # They parsed the agent workflow scripts, which are executable assets nothing else in this gate
    # runs -- so a syntax error was invisible until somebody invoked one, and its symptom was not
    # "the workflow is broken" but "people hand-rolled the fan-out it exists to replace": the
    # translate workflow was unparseable for weeks over ONE unescaped apostrophe, and a batch
    # hand-rolled twelve translations in between. Parse-only, because a lint that spawns agents is a
    # lint people disable. The workflows are a private layer; on a public tree the row found zero
    # scripts and returned 0 -- it passed VACUOUSLY, which is not a gate, and is the second reason
    # it leaves rather than being skipped.
    # LIB-CONSTSIG. mh_calls.gen.h and mh_export.gen.h each describe every original function, from
    # different generators, and nothing made them agree. The drift is invisible until a THIRD
    # consumer has to satisfy both at once -- it surfaced twice that way, as two compile errors in
    # rx_dispatch.cpp and as four unresolved externals when tact left the rebind's deferred set.
    # This compares the two headers directly, so no consumer has to exist. The call-vs-entry const
    # difference on by-value blob params is the documented RULE and is encoded, not reported.
    check(
        "one callee, one committed shape (lint_proto_shapes)",
        [sys.executable, os.path.join(REPO, "tools", "lint_proto_shapes.py"), "--check"],
    )
    check(
        "proto-shape drift -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_proto_shapes.py"), "--selftest"],
    )
    # BS-ENV E1. tools/'s Python deps were declared nowhere, so a fresh machine could not install what
    # tools/ needs without reading every script. tools/requirements.txt now pins them -- but a
    # hand-maintained dependency list rots the first time someone adds an `import` without updating it.
    # This re-derives the third-party import set from the tree and fails if anything imported is not
    # pinned. Pure text + installed package metadata, no Ghidra.
    check(
        "python deps pinned (lint_requirements)",
        [sys.executable, os.path.join(REPO, "tools", "lint_requirements.py")],
    )
    # tooling:TL-SUITE-RESREG. 12+ separate incidents (TL-RIG6/7/8/11, TL-LOCKRACE, TL-POLITELOCK,
    # TL-LANECOLLIDE, TL-SHIMCTL, TL-SELFTEST-STAGE, TL-TMPLEAK, G268, hostlock reaping a live
    # holder) were all one shape: a literal port/%TEMP% dir/lock/mutex name that two forms of
    # parallelism both bind, fixed one at a time. tools/data/resource_registry.json names every
    # such literal tools/*.py and *.bat actually uses and how it is scoped; this refuses any literal
    # the registry does not account for. Rig-free, sub-second.
    check(
        "every machine-global resource literal is registered (lint_resources)",
        [sys.executable, os.path.join(REPO, "tools", "lint_resources.py")],
    )
    check(
        "resource-literal scan -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_resources.py"), "--selftest"],
    )
    # TL-SUITE-INIMERGE: the rig's [net] defaults must match the shipped example ini unless declared
    # (tools/data/rig_ini_default_exceptions.json) -- the TL-RIG-DEFANG shape.
    check(
        "rig [net] defaults match the shipped ini (lint_rig_ini_defaults)",
        [sys.executable, os.path.join(REPO, "tools", "lint_rig_ini_defaults.py")],
    )
    check(
        "rig-defaults lint -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_rig_ini_defaults.py"), "--selftest"],
    )
    # tooling:TL-SUITE-LOADRED. A suite red under contention (TL-HARN19: 51s alone vs 424s killed in
    # the full suite) is auto-rerun alone once; the allow-list that can accept a proven load flake
    # instead of failing the gate forever needs its own dates checked, or it silently accepts one
    # nobody re-dates (the TL-HARN19/TL-GATE-LOADFLAKE-0925 shape, one level up).
    check(
        "the solo-rerun allow-list has no expired/malformed entry (load_red_allow)",
        [sys.executable, os.path.join(REPO, "tools", "load_red_allow.py"), "--check"],
    )
    check(
        "solo-rerun allow-list -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "load_red_allow.py"), "--selftest"],
    )
    check(
        "solo rerun of red suite rows -- the negative cases still fire (test_ui --loadred-selftest)",
        [sys.executable, os.path.join(REPO, "tools", "test_ui.py"), "--loadred-selftest"],
    )
    # tooling:TL-SUITE-LOADRED (b). A capture/assertion step or a peer-dependent shim trigger whose
    # only synchronization is a wall/game-clock offset (TL-P9W-TRIGGER's fixed t+50s blackhole,
    # ~97% odds of landing inside a barrier and usually missing) is refused; state predicates,
    # shim_triggers log-line gates and simstep fences are the allowed alternatives.
    check(
        "no clock-only sync in a uiscript or a shim_timeline row (lint_ui_sync)",
        [sys.executable, os.path.join(REPO, "tools", "lint_ui_sync.py")],
    )
    check(
        "clock-only-sync lint -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_ui_sync.py"), "--selftest"],
    )
    # tooling:TL-SUITE-COUNTERS. Dead-end G101: a counter reported only at graceful shutdown is
    # invisible to a rig run, which is always KILLED. Refuses a NEW `g_*` counter whose only print is
    # inside a shutdown-path function/message; the shared first-hit + periodic-rollup helper is the
    # fix (mh_diag_counter.h, mh_common's headers). Rig-free, sub-second.
    check(
        "no counter reported only at teardown (lint_shutdown_counters)",
        [sys.executable, os.path.join(REPO, "tools", "lint_shutdown_counters.py")],
    )
    check(
        "shutdown-only-counter lint -- the planted positives/negatives still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_shutdown_counters.py"), "--selftest"],
    )
    # BS-ENV E2b. E2 lifted machine constants out of tools/*.py; E2b did the same for src/formats +
    # a plain REPO-hardcoding tool, and made the DLL build config portable. This fails if a bare
    # machine-specific path (a game-install or tools-install drive root, a LAN IP, a hardcoded VS
    # root) reappears in the cleaned set. Widened at fork F5B to the whole publish set, where it
    # also refuses a public host, a user-profile path and an operator identity literal.
    # Rig-free, sub-second.
    check(
        "no reintroduced machine paths (lint_machine_paths)",
        [sys.executable, os.path.join(REPO, "tools", "lint_machine_paths.py")],
    )
    check(
        "machine-path scan -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_machine_paths.py"), "--selftest"],
    )
    # dist DS1. THE RUST HALF OF THE TREE. src/launcher and src/relay are Rust; clang-format and
    # ruff cannot see them, so without these two rows the only two crates in the repo would be the
    # only source in it under no formatter and no linter at all. `-D warnings` is what makes the
    # clippy row a gate rather than a report. Both go through tools/lint_rust.py for its
    # three-valued verdict (see rust_row above): on a machine with no cargo they SKIP, by name,
    # with the remedy -- they never silently pass. Same lane, because both drive cargo against one
    # shared target/ directory and cargo takes a lock on it.
    check_fn(
        "rust fmt (lint_rust --fmt)", rust_row("--fmt", "cargo fmt --all --check"), lane="cargo"
    )
    check_fn(
        "rust clippy -D warnings (lint_rust --clippy)",
        rust_row("--clippy", "cargo clippy --workspace --all-targets -- -D warnings"),
        lane="cargo",
    )
    check(
        "rust lint verdict -- the negative cases still fire",
        [sys.executable, os.path.join(REPO, "tools", "lint_rust.py"), "--selftest"],
    )
    # F5D. THIS FILE'S OWN --ci SUBSET, armed. The dead-skip arm runs inline on every lint run (see
    # check_ci_skip_list), so what is left to prove is that the arm can FIRE and that a skipped row
    # is NAMED rather than merely absent -- a --ci that silently dropped a row would look exactly
    # like a --ci that ran it. Declares the roster and runs nothing, so it costs a tree walk.
    check(
        "fork F5D --ci subset -- the negative cases still fire (lint_repo --ci-selftest)",
        [sys.executable, os.path.join(REPO, "tools", "lint_repo.py"), "--ci-selftest"],
    )
    # IF THIS LANE FAILS WITH "Configuration file(s) do(es) not support Objective-C" AND NAMES NO
    # FILE, the cause is clang-format's LANGUAGE GUESSER, not a formatting violation. A `.h` whose
    # content it reads as Objective-C is rejected outright against our Cpp-only .clang-format, and
    # because the lane runs in batches the message arrives without the filename. The trigger seen so
    # far (2026-09-08, crt/crt_math.h): an `@0x004e9285`-style VA inside an inline-`__asm` comment --
    # the `@` reads as an ObjC keyword. Spell VAs without the `@` inside asm blocks. To find the
    # offender, run CLANG_FORMAT --dry-run -Werror over the changed files ONE AT A TIME.
    if args.fix:
        check_chunked(f"clang-format -i ({len(files)} files)", [CLANG_FORMAT, "-i"], files)
        check("ruff format tools", ["ruff", "format", "tools"])
    else:
        check_chunked(
            f"clang-format --dry-run -Werror ({len(files)} files)",
            [CLANG_FORMAT, "--dry-run", "-Werror"],
            files,
        )
        check("ruff format --check tools", ["ruff", "format", "--check", "tools"])


def check_ci_skip_list():
    """THE DEAD-SKIP ARM. Every CI_SKIPS key must name a row the roster actually declares.

    A skip list is an exemption list, and the way an exemption list rots is that the check it
    excused was renamed or deleted and the entry stayed -- at which point --ci silently covers one
    row fewer than its own table claims, and the table is no longer readable as "what the public
    gate does not cover". Run in BOTH modes on purpose, so the private tree's ordinary lint is what
    catches it. Returns a list of problems (empty == ok)."""
    roster = set(_DECLARED)
    return ["declared --ci skip names no such row: %r" % n for n in CI_SKIPS if n not in roster]


def _reset_roster():
    del _QUEUE[:]
    del _DECLARED[:]
    del _SKIPPED[:]


def ci_selftest():
    """Planted negatives for the --ci machinery itself. Costs no check run -- it only DECLARES."""
    global CI_MODE
    ap_args = argparse.Namespace(fix=False, jobs=None, ci=False)
    fails = []

    def arm(ok, label, detail=""):
        print("  [%s] %s%s" % ("ok" if ok else "FAIL", label, ("  -- " + detail) if detail else ""))
        if not ok:
            fails.append(label)

    # 1. The real list is live: every declared skip names a row that exists.
    CI_MODE = False
    _reset_roster()
    declare_checks(ap_args)
    roster_size = len(_DECLARED)
    problems = check_ci_skip_list()
    arm(not problems, "every declared --ci skip names a live row", "; ".join(problems))
    arm(roster_size > 100, "the roster is non-vacuous", "%d rows declared" % roster_size)

    # 2. ...and the arm that says so can actually fire (a name nobody declares).
    CI_SKIPS["__planted__ a row that does not exist"] = "planted"
    try:
        arm(
            len(check_ci_skip_list()) == 1,
            "a skip naming a dead row is REFUSED",
            "the dead-skip arm did not fire",
        )
    finally:
        del CI_SKIPS["__planted__ a row that does not exist"]

    # 3. Without --ci nothing is skipped: the exemption list cannot leak into the normal gate.
    arm(not _SKIPPED, "no row is skipped without --ci", "%d skipped" % len(_SKIPPED))
    arm(len(_QUEUE) == roster_size, "every declared row is queued without --ci")

    # 4. With --ci, every declared skip is REPORTED and none of them is queued.
    CI_MODE = True
    _reset_roster()
    try:
        declare_checks(ap_args)
    finally:
        CI_MODE = False
    reported = {n for n, _ in _SKIPPED}
    queued = {c.name for c in _QUEUE}
    arm(reported == set(CI_SKIPS), "every declared skip is reported by name in --ci")
    arm(not (reported & queued), "no reported skip is also queued")
    arm(
        len(_DECLARED) == roster_size and len(_QUEUE) == roster_size - len(CI_SKIPS),
        "the printed arithmetic holds",
        "total=%d run=%d skipped=%d" % (len(_DECLARED), len(_QUEUE), len(_SKIPPED)),
    )
    # 4b. THE TREE DISPATCH (fork F5M). The publish-ledger rows are not in CI_SKIPS any more: they
    #     choose their arm from the tree. Both branches are declared here so neither can rot into a
    #     row that never runs -- the public branch has no tree to run on during a private lint, and
    #     a dispatch nobody exercises is exactly the silently-vacuous row this file is written
    #     against. Forced, both ways, with the detector pinned.
    global _TREE_IS_CUT
    saved_tree = _TREE_IS_CUT
    try:
        rosters = {}
        for kind, value in (("private", False), ("public", True)):
            _TREE_IS_CUT = value
            _reset_roster()
            declare_checks(ap_args)
            rosters[kind] = set(_DECLARED)
        arm(
            len(rosters["private"]) == len(rosters["public"]) == roster_size,
            "the tree dispatch declares the same NUMBER of rows either way",
            "private=%d public=%d" % (len(rosters["private"]), len(rosters["public"])),
        )
        only_priv = rosters["private"] - rosters["public"]
        only_pub = rosters["public"] - rosters["private"]
        arm(
            len(only_priv) == 2 and len(only_pub) == 2,
            "the publish-ledger rows, and only those, differ by tree",
            "private-only=%d public-only=%d" % (len(only_priv), len(only_pub)),
        )
        arm(
            all("--public" in n for n in only_pub),
            "the public-tree rows NAME the arm they run",
            "; ".join(sorted(only_pub)),
        )
        arm(
            not (set(CI_SKIPS) & (only_priv | only_pub)),
            "neither tree's publish-ledger row is ALSO in the skip list",
        )
    finally:
        _TREE_IS_CUT = saved_tree
        _reset_roster()
        CI_MODE = True
        try:
            declare_checks(ap_args)
        finally:
            CI_MODE = False

    # 5. ...and a skipped row is not merely absent from the queue but NAMED in the emitted text.
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        report_skips()
    text = buf.getvalue()
    missing = [n for n in CI_SKIPS if n not in text]
    arm(not missing, "the emitted --ci report names every skipped row", "; ".join(missing[:3]))

    print("lint_repo --ci-selftest:", "PASS" if not fails else "FAIL (%d)" % len(fails))
    return 0 if not fails else 1


def report_skips():
    if not _SKIPPED:
        return
    print(
        "lint_repo --ci: %d row(s) held back -- each named here, none silently dropped"
        % len(_SKIPPED)
    )
    for name, reason in _SKIPPED:
        print("SKIPPED (ci): %s -- needs %s" % (name, reason))
    print()


def main():
    global CI_MODE
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--fix", action="store_true", help="apply formatting instead of checking")
    ap.add_argument(
        "--jobs",
        type=int,
        default=None,
        help="parallel lanes (default: CPU count). --jobs 1 restores the old sequential run.",
    )
    ap.add_argument(
        "--ci",
        action="store_true",
        help="the PUBLIC-tree subset: skip the CI_SKIPS rows, naming each one in the output",
    )
    ap.add_argument(
        "--ci-selftest",
        action="store_true",
        help="planted negatives for the --ci skip machinery itself (declares only, runs nothing)",
    )
    args = ap.parse_args()
    if args.ci_selftest:
        return ci_selftest()
    CI_MODE = args.ci

    ok = True
    declare_checks(args)
    # The skip list is checked against the roster in BOTH modes -- see check_ci_skip_list.
    stale = check_ci_skip_list()
    for problem in stale:
        print("[FAIL] --ci skip list: %s" % problem)
    ok &= not stale
    report_skips()
    ok &= run_queued(1 if args.fix else args.jobs)
    print(
        "lint_repo: rows total=%d run=%d skipped=%d%s"
        % (
            len(_DECLARED),
            len(_DECLARED) - len(_SKIPPED),
            len(_SKIPPED),
            " (--ci)" if CI_MODE else "",
        )
    )
    print("lint_repo:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
