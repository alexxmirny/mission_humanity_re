#!/usr/bin/env python3
"""Build the PUBLIC SEED -- fork F5M S5/S6: the publish set, its own VCS config, one commit.

    python tools/build_public_seed.py --out  <dir>          # materialize only
    python tools/build_public_seed.py --seed <dir>          # materialize + git init + one commit
    python tools/build_public_seed.py --seed <dir> --push <remote-url>
    python tools/build_public_seed.py --check              # the derivation drift gate (lint_repo)
    python tools/build_public_seed.py --update-config      # rewrite the committed snapshots
    python tools/build_public_seed.py --selftest           # the planted negatives

WHAT THIS IS. F5C's one-off proof harness copied `check_publishable.py --publish-list` into a
scratch tree to prove the build needs nothing the ledger withholds. That was a measurement. This is
the CUT ITSELF, and the difference is everything it adds around the copy: a fail-closed preflight,
a DERIVED `.gitignore`/`.gitattributes` (below), a re-scan of the materialized BYTES for operator
identity, the git identity the seed is authored under, and an after-the-fact proof that the repo it
just built tracks exactly the publish set and nothing else.

FAIL CLOSED, IN THIS ORDER, BEFORE A SINGLE FILE IS COPIED. A cut is irreversible once pushed, so
every question that can be asked of the PRIVATE tree is asked there first and a `no` stops the run:

  1. `git status --porcelain` must be empty. The publish set is derived from `git ls-files`, so an
     uncommitted edit means the seed carries content no commit in the private repo describes -- and
     the ledger row that dispositions it may be the thing being edited.
  2. `check_publishable.py` must PASS -- every tracked path dispositioned, no dead rule.
  3. `check_publishable.py --closure` must PASS -- no published source needs a withheld path.
  4. `lint_machine_paths.py` must PASS -- the publish set carries no operator identity.

Each runs as a subprocess so the verdict is the SAME verdict lint_repo reports, not a re-derivation
that could drift from it. `check_publishable` is imported as a library for the path work (one ledger,
one glob resolver) and shelled out to only for these verdicts.

---- THE DERIVED VCS CONFIG, AND WHY IT IS DERIVED RATHER THAN COPIED ---------------------------

The ledger publishes `.gitignore` and `.gitattributes` verbatim for one stated reason: a clone
without them cannot run the lint it is asked to pass. True, and it stops one step short. Copied
verbatim, the public tree's `.gitignore` opens with an 18-line comment narrating the Ghidra project
database -- its size, the day it was purged, the rollback protocol -- for a directory that cannot
exist in a tree whose entire Ghidra toolchain is withheld, and `.gitattributes` carries five
directory rules protecting that same database. Roughly thirty lines describing private machinery,
shipped as configuration.

So both files are DERIVED here from a declared, per-entry disposition table, and the derivation is
fail-closed in the way that matters: an entry in the private file with no row in the table STOPS THE
BUILD. Adding a rule to `.gitignore` therefore forces a decision about the public tree, which is the
only mechanism that keeps a derivation honest once its author has moved on.

  KEEP means the rule can match a path a public clone PRODUCES -- named, per row, in `why`
       (a published tool's default output directory, a build output, a standard editor/OS artifact).
  DROP means the rule's only subject is private, and the row proves it one of two ways:
       `withheld_by` names a ledger rule, checked to exist, to be non-publish, and to still own
       tracked paths; or `no_producer` declares that nothing in the tree creates the path at all,
       checked by requiring git to track nothing under the entry's literal prefix.
  A KEEP row carrying `withheld_by` is a contradiction and is REFUSED -- that is the planted
       negative `--selftest` fires: an entry whose subject is withheld must not survive derivation.

The eol policy is kept in substance, comment and all: the reason it states -- Git for Windows ships
`core.autocrlf=true` in its SYSTEM gitconfig, so a bare `text` attribute gives a Windows clone a CRLF
working tree against an LF blob, and a file whose size differs from its blob is a stat-cache landmine
that reports `M` forever while `git diff` shows nothing -- is not private history. It is exactly what
a public clone on Windows needs to know, and it is why the attribute says `eol=lf` rather than
leaving it to the user's git config.

The derived files are installed INTO THE SEED ONLY. The private tree keeps its own, unchanged. The
committed snapshots under `tools/data/public_repo_config/` are what `--check` diffs against, so the
derivation is drift-gated like every other generated artifact here; that row is tree-dispatched,
because in the public tree the snapshots' counterpart is the checkout's own `.gitignore`.

---- WHAT THE SEED PROVES ABOUT ITSELF ---------------------------------------------------------

After the commit, three questions are asked of the repository that now exists, not of the copy loop
that built it:

  * `git ls-files` in the seed equals the publish set, exactly. This is the arm that catches a
    derived ignore rule swallowing a published file -- the silent failure, invisible to any amount
    of looking at what IS there. (`mod_src/cfg/*` plus `!mod_src/cfg/aliases.yaml` is one real
    ordering dependency; this proves the pair survived derivation in the right order.)
  * `check_publishable.py --public`, run from the SEED's own copy of the tool, so `REPO` resolves to
    the seed: no withheld path tracked, no publish rule left matching nothing.
  * the materialized bytes carry no public IPv4, no user-profile path, no derived identity token and
    no machine drive path -- `lint_machine_paths.scan_published`, re-run over the destination. The
    patterns come from that module and the tokens from `git config` at run time; nothing this file
    contains is a secret, which is the only way to write a scanner for one.

`--push` exists so the last step is the same code path as the rehearsal, and it does nothing unless
given a URL. Point it at a throwaway bare repo first, and set that repo's HEAD to the seed branch
before cloning: `git init --bare` leaves HEAD on `master`, and a clone of a bare repo whose HEAD
names an unborn branch fails with "does not have any commits yet" while the objects are all there.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

import check_publishable as cp  # noqa: E402

CONFIG_DIR = os.path.join(REPO, "tools", "data", "public_repo_config")
SNAPSHOTS = {"gitignore": ".gitignore", "gitattributes": ".gitattributes"}

SEED_BRANCH = "main"

# THE SEED'S AUTHOR IS NOT WRITTEN DOWN HERE, and that is not squeamishness -- this file is
# PUBLISHED, and lint_machine_paths' identity arm reds an operator identity anywhere in the publish
# set. So the NAME is read from the one place the project already spells it, LICENSE's copyright
# line (the single site that lint's allowance D excuses), which also makes the commit and the
# copyright agree by construction rather than by two people remembering the same string. The EMAIL
# has no such home, so it is an argument: --author-email, or MH_SEED_AUTHOR_EMAIL. The operator
# runbook carries the ruled value.
LICENSE_HOLDER_RE = re.compile(r"^\s*Copyright \(c\) \d{4}(?:-\d{4})? +(.+?)\s*$", re.MULTILINE)

# The seed commit's message. First paragraph: README.md's first screen, compressed to the two claims
# it opens with. Second: where the history went, stated rather than left to be inferred from a repo
# whose first commit is 2,367 files.
SEED_COMMIT_MESSAGE = """mh: a re-implemented engine core for Exterminacja / Mission Humanity

The 2001 Techland RTS, with its multiplayer restored and its deterministic
simulation spine re-implemented in C++ and executed in-process: an injected DLL
supplies the network transport the retail build never wired to a socket, and a
growing share of the order pipeline, lockstep and save/load runs as our code
inside the original executable, gated against the original's behaviour.

Seed commit of the public tree, derived from the private research repository by
tools/build_public_seed.py; history begins here by design.
"""


def license_holder(root=REPO):
    """The copyright holder LICENSE names, which is the seed commit's author name."""
    path = os.path.join(root, "LICENSE")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as fh:
        m = LICENSE_HOLDER_RE.search(fh.read())
    return m.group(1).strip() if m else None


def author_email(explicit=None):
    return (explicit or os.environ.get("MH_SEED_AUTHOR_EMAIL") or "").strip()


# --------------------------------------------------------------------------- the derivation table


def _keep(entry, group, why):
    return {"entry": entry, "disposition": "keep", "group": group, "why": why}


def _drop(entry, why, withheld_by=None, no_producer=None):
    row = {"entry": entry, "disposition": "drop", "why": why}
    if withheld_by:
        row["withheld_by"] = withheld_by
    if no_producer:
        row["no_producer"] = no_producer
    return row


# Group order in the derived file. Within a group, entries keep their order in the private file --
# which is what preserves `mod_src/cfg/*` ahead of its `!` negation.
IGNORE_GROUPS = [
    ("scratch", "scratch, staging and stray files"),
    ("retail", "retail game binaries -- never committed (mhpatch takes the exe as an argument)"),
    ("mod", "mod trees: cfgkit / mapkit output. aliases.yaml is the one versioned file in cfg/"),
    ("build", "build outputs"),
    ("editor", "editor and IDE state"),
    ("local", "machine-local configuration and per-run artifacts"),
]

GITIGNORE_ROWS = [
    _drop(
        "db/",
        "The Ghidra project database. It is untracked in the private repository too, and every "
        "tool that opens a Ghidra project is archive-class, so no path in this tree creates it.",
        withheld_by="archive-class-tools",
    ),
    _keep(
        "workdir/",
        "scratch",
        "In-tree game-run lanes: tools/gate_timeline.py reads workdir/mh_lanes/*/logs/*_solo and "
        "tools/gen_st0_goldens.py takes a run log from the same tree. Staging a retail install "
        "there is exactly what must never be committed.",
    ),
    _keep(
        "game_data/",
        "scratch",
        "Staged game-tree inputs (the retail install INSTALL.md has the reader supply). Kept for "
        "the same reason as workdir/: a never-commit-the-game guard costs one line and dropping "
        "one is the only irreversible direction.",
    ),
    _keep(
        "tmp*.ps",
        "scratch",
        "Stray PowerShell transcripts a redirect drops at the repo root on Windows.",
    ),
    _drop(
        "*.grf",
        "Ghidra's transient recovery snapshots for unsaved in-memory changes -- written by Ghidra "
        "into its project directory, which this tree does not carry.",
        withheld_by="archive-class-tools",
    ),
    _keep(
        "tmp/",
        "scratch",
        "The scratch output directory every tool writes under (tmp/gate's timings, tmp/cov's "
        "cobertura XML, the harness capture dirs).",
    ),
    _keep(
        "mod_src/overlay/",
        "mod",
        "The compiled overlay tree: the default --out of src/formats/cfgkit/cfg_compile.py and "
        "src/formats/mapkit/map_compile.py.",
    ),
    _keep(
        "mod_src/cfg/*",
        "mod",
        "The decompiled retail cfg tree: the default --out of "
        "src/formats/cfgkit/cfg_decompile.py. Game-derived, regenerable, never committed -- the "
        "contents are ignored rather than the directory so the negation below can re-include one "
        "file.",
    ),
    _keep(
        "!mod_src/cfg/aliases.yaml",
        "mod",
        "LOAD-BEARING: aliases.yaml is published and tracked, and it lives inside the ignored "
        "cfg/ tree. Without this line the seed's own `git add` would drop it.",
    ),
    _keep(
        "mod_src/map/",
        "mod",
        "The Tiled export tree: the default --out of src/formats/mapkit/map_decompile.py.",
    ),
    _keep(
        "src/patcher/mh.exe",
        "retail",
        "A retail binary dropped next to the patcher. mhpatch takes the exe as a command-line "
        "argument, so neither file normally exists -- these are the defensive half of the "
        "never-commit-the-game rule.",
    ),
    _keep("src/patcher/mh_.exe", "retail", "As above: the second conventional name."),
    _keep("src/**/Debug/", "build", "MSBuild output directory (Debug|Win32)."),  # CITATION-OK
    _keep(
        "src/**/Release/",  # CITATION-OK
        "build",
        "MSBuild output directory (Release|Win32), the gate's build.",
    ),
    _keep(
        "src/**/Release_asan/",  # CITATION-OK
        "build",
        "The ASan configuration's own intermediate + output tree; `Release/` does not cover it, a "
        "gitignore path component is exact rather than a prefix.",
    ),
    _keep("src/**/build/", "build", "Out-of-tree build directories."),  # CITATION-OK
    _keep(
        "dist/",
        "build",
        "The release packager's output tree. tools/release_package.py assembles the three "
        "drop-in zips and a SHA256SUMS there by default, and release.yml uploads it as a "
        "workflow artifact; it is regenerable from a built Release tree in one command.",
    ),
    _keep(
        "/target/",
        "build",
        "The cargo workspace's build tree (dist DS1). KEPT rather than dropped because the "
        "workspace itself publishes -- Cargo.toml, Cargo.lock, rust-toolchain.toml and both member "
        "crates under src/ are in the publish set -- so a public clone that runs `cargo build`, "
        "which INSTALL.md tells it how to do, creates this directory. Rooted (`/target/`) rather "
        "than bare so it names the one workspace target dir and not any directory called target.",
    ),
    _keep("src/**/x64/", "build", "The x64 platform's output tree."),  # CITATION-OK
    _keep("src/**/.vs/", "build", "Visual Studio's per-solution cache under src/."),  # CITATION-OK
    _keep("src/**/*.obj", "build", "MSVC object files."),  # CITATION-OK
    _keep("src/**/*.pdb", "build", "MSVC debug databases."),  # CITATION-OK
    _keep("src/**/*.ilk", "build", "MSVC incremental-link state."),  # CITATION-OK
    _keep("src/**/*.idb", "build", "MSVC incremental-compile state."),  # CITATION-OK
    _keep("src/**/*.user", "build", "Per-developer Visual Studio project settings."),  # CITATION-OK
    _keep(
        "src/**/__pycache__/",  # CITATION-OK
        "build",
        "Python bytecode caches under src/ (src/formats).",
    ),
    _keep(
        "*.patched.exe",
        "retail",
        "mhpatch's output: a retail executable with our patches applied. INSTALL.md's patch step "
        "produces one.",
    ),
    _keep("*.harness.exe", "retail", "The UI harness's patched variant of the same."),
    _keep(
        ".obsidian/",
        "editor",
        "Obsidian's per-vault state, written by the editor into any markdown tree it is opened on.",
    ),
    _keep(
        "bash.exe*",
        "scratch",
        "A stray Git-for-Windows shim a mistyped redirect can leave at the repo root.",
    ),
    _keep("__pycache__/", "build", "Python bytecode caches anywhere in the tree."),
    _keep("*.pyc", "build", "As above, for a cache written outside a __pycache__ directory."),
    _keep(".vs/", "editor", "Visual Studio's solution cache, created the first time mh.sln opens."),
    _keep(".vscode/", "editor", "Visual Studio Code workspace settings."),
    _drop(
        "session_reports/loop/",
        "The unattended migration driver's run telemetry (lockfile, heartbeat, per-session logs). "
        "The research hand-off tree it sits under is a private layer wholesale, and the driver "
        "that writes this subdirectory is archive-class.",
        withheld_by="private-research-layers",
    ),
    _drop(
        "metadata/",
        "The Ghidra metadata export. Its only producer is an archive-class dumper, so nothing "
        "this tree carries writes the directory.",
        withheld_by="archive-class-tools",
    ),
    _keep(
        "tools/machine.local.json",
        "local",
        "The per-machine override file tools/machine_config.py reads and bootstrap.py tells the "
        "reader to write. Ignoring it is what keeps one box's paths out of everyone's clone.",
    ),
    _drop(
        ".claude/settings.local.json",
        "Local settings for the agent harness, whose configuration tree is a private research "
        "layer wholesale.",
        withheld_by="private-research-layers",
    ),
    _keep(
        "LastCoverageResults.log",
        "local",
        "OpenCppCoverage drops this in the working directory on every tools/coverage.py run; the "
        "real output is the cobertura XML under tmp/cov/.",
    ),
    _drop(
        "tools/uiscripts/journals/raw/",
        "Preserved raw human-recording run directories -- large session archives kept by a "
        "file-level backup, never committed. Nothing writes the directory: the recorder's run "
        "dirs land under tmp/, and these were placed by hand.",
        no_producer="git tracks nothing under tools/uiscripts/journals/raw/",
    ),
]

GITIGNORE_HEADER = """\
# DERIVED -- do not hand-edit. `python tools/build_public_seed.py --check` fails if this file and
# the derivation disagree.
#
# This repository is a cut of a larger private research tree, and its .gitignore is derived from
# that tree's rather than copied: every rule that can match a path THIS tree produces is kept, with
# a one-line reason; the rules whose only subject is a private layer (the Ghidra project database,
# the research telemetry) are dropped along with the narration explaining them. A rule the
# derivation has no disposition for stops the build, so the question cannot be skipped.
"""

ATTR_GROUPS = [
    ("lf", None),
    ("def", None),
    ("crlf", None),
    ("binary", None),
]

# Text that precedes a group in the derived .gitattributes. The eol reason is kept in substance:
# it is a property of git on Windows, not of the private repository.
ATTR_GROUP_TEXT = {
    "lf": """\
# -- text sources: LF in the repo AND LF in the working tree --
#
# `eol=lf` is explicit rather than left to core.autocrlf ON PURPOSE. Git for Windows ships
# `core.autocrlf=true` in its SYSTEM gitconfig, so a bare `text` attribute gives every clone on
# Windows a CRLF working tree against an LF blob -- and a working file whose size differs from its
# blob is a stat-cache landmine: a branch checkout can leave the index caching the BLOB's size,
# after which git short-circuits on the size mismatch and reports the file modified WITHOUT hashing
# it. `git status` then says `M` forever while `git diff` says nothing, and `git update-index
# --refresh` cannot repair it (plain --refresh is stat-only). Stating eol here makes it a property
# of the REPOSITORY, so it holds for a fresh clone on a box nobody controls.""",
    "def": """\
# The hand-derived module-definition files: LF blobs whose drift gate (gen_harness_contract --check
# / gen_libmh_contract --check) compares rendered text against the checkout. Pinned per PATH, not
# `*.def`, because libmh_std.def and proxy_exports.gen.def are CRLF blobs written that way by their
# generators, and renormalising them would flip THEIR gates.""",
    "crlf": """\
# -- Visual Studio project files: CRLF, because VS writes them that way --
#
# Deliberately narrow. Only *.vcxproj / *.filters are genuinely CRLF, and only because Visual Studio
# rewrites them on save; LF there would mean permanent churn. Everything else Windows-flavoured in
# this tree (the .bat, the .sln, the .ini fragments the DLL parses at runtime) works LF -- measured,
# not assumed -- so the exception leaves six files carrying a working-tree size that differs from
# their blob rather than sixty.""",
    "binary": """\
# -- binary artifacts: byte-verbatim, never eol-converted --
#
# The game's own binary config and the seed the UI harness copies from, the Watcom function-ID
# databases, and the file types a build or a capture can drop into the tree. A blanket
# `* text=auto` is deliberately NOT used anywhere in this file: a misdetected binary getting
# CRLF-normalised on commit round-trips only by accident, and stops doing so the moment someone
# clones with a different autocrlf setting.""",
}

GITATTRIBUTES_ROWS = [
    _keep("*.cpp     text eol=lf", "lf", "C++ sources: 1,047 published files."),
    _keep("*.c       text eol=lf", "lf", "C sources (the host-API compile check)."),
    _keep("*.h       text eol=lf", "lf", "Headers: 743 published files."),
    _keep("*.py      text eol=lf", "lf", "Python: the tools and src/formats."),
    _keep("*.md      text eol=lf", "lf", "Prose: README, INSTALL, docs/."),
    _keep("*.json    text eol=lf", "lf", "Generated-and-gated data under tools/data/."),
    _keep("*.yaml    text eol=lf", "lf", "mod_src/cfg/aliases.yaml."),
    _keep("*.yml     text eol=lf", "lf", ".github/workflows/ci.yml."),
    _keep("*.js      text eol=lf", "lf", "The TTD trace scripts under tools/ttd/."),
    _keep("*.toml    text eol=lf", "lf", "ruff.toml."),
    _keep("*.txt     text eol=lf", "lf", "UI harness scripts and fixtures."),
    _keep("*.ini     text eol=lf", "lf", "The ini fragments the DLL parses at runtime."),
    _keep("*.html    text eol=lf", "lf", "The session-report template."),
    _keep(
        "src/mh_dll/mh/mh.def                 text eol=lf", "def", "Hand-derived, gate-compared."
    ),
    _keep(
        "src/mh_dll/mh_harness/mh_harness.def text eol=lf", "def", "Hand-derived, gate-compared."
    ),
    _keep(
        "src/mh_dll/mh_net/mh_net.def         text eol=lf", "def", "Hand-derived, gate-compared."
    ),
    _keep(
        "src/mh_dll/libmh_dll/libmh.def       text eol=lf", "def", "Hand-derived, gate-compared."
    ),
    _keep("*.vcxproj text eol=crlf", "crlf", "12 published project files Visual Studio rewrites."),
    _keep("*.filters text eol=crlf", "crlf", "12 published filter files, same reason."),
    _drop(
        "**/idata/**      -text",
        "Ghidra project-database internals. The directory rules are the load-bearing half of the "
        "private tree's binary policy and they protect a database this tree does not carry.",
        withheld_by="archive-class-tools",
    ),
    _drop(
        "**/versioned/**  -text",
        "Ghidra's own version store (db.<N>.gbf plus reverse deltas).",
        withheld_by="archive-class-tools",
    ),
    _drop("**/user/**      -text", "Ghidra project user state.", withheld_by="archive-class-tools"),
    _drop(
        "**/project.prp   -text", "Ghidra project properties.", withheld_by="archive-class-tools"
    ),
    _drop("**/projectState  -text", "Ghidra project state.", withheld_by="archive-class-tools"),
    _keep(
        "setup.dat     -text",
        "binary",
        "The game's binary config, written into a lane by the UI harness.",
    ),
    _keep("*.seed.dat    -text", "binary", "tools/uiscripts/setup.seed.dat, the harness's seed."),
    _drop(
        "*.gbf     -text",
        "A Ghidra database file. The extension exists only inside a Ghidra project.",
        withheld_by="archive-class-tools",
    ),
    _drop(
        "*.prp     -text",
        "A Ghidra property file, same scope as the directory rules above.",
        withheld_by="archive-class-tools",
    ),
    _drop(
        "*.rep     -text",
        "The Ghidra project directory suffix.",
        withheld_by="archive-class-tools",
    ),
    _keep(
        "*.fidb    -text", "binary", "The three Watcom function-ID databases under tools/ghidra/."
    ),
    _drop(
        "*.sla     -text",
        "Ghidra's compiled SLEIGH specification. It lives in the Ghidra install; the published "
        "sleigh work ships a .sinc and a .patch, never a compiled .sla.",
        withheld_by="archive-class-tools",
    ),
    _keep("*.dll     -text", "binary", "A built DLL, if one is ever added deliberately."),
    _keep("*.exe     -text", "binary", "As above for an executable."),
    _keep("*.patch   -text", "binary", "tools/ghidra/sleigh_memcpy/rep-movs-memcpy.patch."),
    _keep("*.png     -text", "binary", "Images (harness captures, documentation figures)."),
    _keep("*.bmp     -text", "binary", "Raw harness captures before conversion."),
]

GITATTRIBUTES_HEADER = """\
# DERIVED -- do not hand-edit. `python tools/build_public_seed.py --check` fails if this file and
# the derivation disagree.
#
# Line-ending + binary policy, SCOPED TO KNOWN EXTENSIONS on purpose (see the binary block at the
# end for why a blanket `* text=auto` is not used here).
"""

TABLES = {
    ".gitignore": (GITIGNORE_ROWS, GITIGNORE_HEADER, IGNORE_GROUPS, None),
    ".gitattributes": (GITATTRIBUTES_ROWS, GITATTRIBUTES_HEADER, ATTR_GROUPS, ATTR_GROUP_TEXT),
}


# --------------------------------------------------------------------------- derivation


def parse_entries(text):
    """The rule lines of a .gitignore/.gitattributes: no blanks, no comments, order preserved."""
    out = []
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        out.append(line.rstrip())
    return out


def _key(entry):
    """Match a table row to a file line ignoring the run of spaces used for column alignment."""
    return re.sub(r"\s+", " ", entry.strip())


def derive(which, source_text, led, tracked, spec=None, say=print):
    """-> (derived_text, audit). Fail-closed: an entry with no disposition row raises.

    `spec` overrides the declared table, and exists for --selftest: the planted negatives must
    fire against a SYNTHETIC table and a SYNTHETIC ledger, or the arms would only be testable in
    the private tree (the public tree carries the DERIVED config, whose entry set no longer
    matches the table, and no withheld path for a drop row to lean on)."""
    rows, header, groups, group_text = spec or TABLES[which]
    table = {}
    for r in rows:
        k = _key(r["entry"])
        if k in table:
            raise SystemExit("derivation: duplicate row for %r" % r["entry"])
        table[k] = r

    entries = parse_entries(source_text)
    seen = set()
    kept, dropped = [], []
    for e in entries:
        k = _key(e)
        row = table.get(k)
        if row is None:
            raise SystemExit(
                "derivation: %s carries the entry %r, which the table in "
                "tools/build_public_seed.py has no disposition for. A new rule must be decided "
                "for the public tree (keep + what it matches there, or drop + whose subject it "
                "is) -- that decision is what this gate exists to force." % (which, e)
            )
        seen.add(k)
        (kept if row["disposition"] == "keep" else dropped).append((e, row))

    stale = sorted(set(table) - seen)
    if stale:
        raise SystemExit(
            "derivation: %d table row(s) name an entry %s no longer carries: %s"
            % (len(stale), which, ", ".join(stale))
        )

    # A keep row may not declare a withheld subject. This is the contradiction the planted negative
    # fires on: an entry whose subject the ledger withholds must not survive derivation.
    for e, row in kept:
        if row.get("withheld_by") or row.get("no_producer"):
            raise SystemExit(
                "derivation: %r is KEPT but declares a private subject (%s) -- an entry whose "
                "only subject is withheld cannot be kept in the public config."
                % (e, row.get("withheld_by") or "no_producer")
            )
        if not row.get("why"):
            raise SystemExit("derivation: kept entry %r carries no reason" % e)

    # A drop row must prove its subject, mechanically.
    by_rule = {r["id"]: r for r in led["rules"]}
    _, owned, _ = cp.classify(tracked, led)
    for e, row in dropped:
        rid, nop = row.get("withheld_by"), row.get("no_producer")
        if not rid and not nop:
            raise SystemExit("derivation: dropped entry %r proves nothing" % e)
        if rid:
            rule = by_rule.get(rid)
            if rule is None:
                raise SystemExit(
                    "derivation: %r names ledger rule %r, which does not exist" % (e, rid)
                )
            if rule["disposition"] == cp.PUBLISHED:
                raise SystemExit(
                    "derivation: %r claims rule %r withholds its subject, but that rule PUBLISHES"
                    % (e, rid)
                )
            if not owned.get(rid):
                raise SystemExit(
                    "derivation: %r leans on ledger rule %r, which owns no tracked path -- a dead "
                    "rule cannot carry a disposition" % (e, rid)
                )
        if nop:
            prefix = e.lstrip("!").rstrip("/")
            if any(p == prefix or p.startswith(prefix + "/") for p in tracked):
                raise SystemExit(
                    "derivation: %r claims nothing produces it, but git tracks paths under %s"
                    % (e, prefix)
                )

    out = [header.rstrip("\n"), ""]
    for gid, label in groups:
        members = [e for e, r in kept if r["group"] == gid]
        if not members:
            continue
        if group_text and gid in group_text:
            out.append(group_text[gid])
        elif label:
            out.append("# %s" % label)
        out.extend(members)
        out.append("")
    text = "\n".join(out).rstrip("\n") + "\n"
    audit = {"kept": [(e, r) for e, r in kept], "dropped": [(e, r) for e, r in dropped]}
    return text, audit


def derive_both(say=print):
    led = cp.load_ledger()
    tracked = cp.tracked_paths()
    out = {}
    for which in SNAPSHOTS.values():
        src = os.path.join(REPO, which)
        with open(src, encoding="utf-8") as fh:
            text = fh.read()
        out[which] = derive(which, text, led, tracked, say=say)
    return out


def snapshot_path(which):
    return os.path.join(CONFIG_DIR, [k for k, v in SNAPSHOTS.items() if v == which][0])


def _read(path):
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8", newline="") as fh:
        return fh.read()


def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def _same(a, b):
    """Byte-equal after newline normalisation. A hosted Windows runner with core.autocrlf checks
    .gitignore/.gitattributes out as CRLF while the derivation and the snapshot are LF, and the
    2026-09-16/18 CI runs failed this row with "differs (0 diff line(s))" -- the line diff was empty
    because only the line ENDINGS differed. Content is what the gate is about."""
    return (a or "").replace("\r\n", "\n") == (b or "").replace("\r\n", "\n")


def _diff(a, b, label, say):
    import difflib

    lines = list(
        difflib.unified_diff(
            (a or "").splitlines(), (b or "").splitlines(), "expected", "found", lineterm="", n=1
        )
    )
    say("  %s differs (%d diff line(s)):" % (label, len(lines)))
    for ln in lines[:40]:
        say("    %s" % ln)
    if len(lines) > 40:
        say("    ... and %d more" % (len(lines) - 40))


def check_config(say=print):
    """THE DRIFT GATE, tree-dispatched.

    PRIVATE tree: re-derive from this checkout's own .gitignore/.gitattributes and require the
    committed snapshots to match -- so a new ignore rule cannot reach the seed undecided.
    PUBLIC tree: the snapshots' counterpart is the checkout's OWN config, which the seed installed;
    require them equal, which is what proves the seed was built by this derivation and not by hand.
    Neither arm is vacuous in the other tree, which is why the row dispatches instead of skipping."""
    led = cp.load_ledger()
    paths = cp.tracked_paths()
    public = cp.is_public_tree(paths, led)
    ok = True
    if public:
        say("build_public_seed --check: PUBLIC tree arm (the installed config is the snapshot)")
        for snap, which in SNAPSHOTS.items():
            want = _read(os.path.join(CONFIG_DIR, snap))
            have = _read(os.path.join(REPO, which))
            if want is None:
                say("  missing snapshot tools/data/public_repo_config/%s" % snap)
                ok = False
            elif not _same(want, have):
                ok = False
                _diff(want, have, which, say)
            else:
                say("  %s matches its snapshot (%d bytes)" % (which, len(want)))
    else:
        say("build_public_seed --check: PRIVATE tree arm (re-derive and compare to the snapshot)")
        for which, (text, audit) in derive_both(say=say).items():
            snap = snapshot_path(which)
            want = _read(snap)
            if want is None:
                say("  missing snapshot %s" % os.path.relpath(snap, REPO).replace("\\", "/"))
                ok = False
            elif not _same(want, text):
                ok = False
                _diff(text, want, which, say)
            else:
                say(
                    "  %s: %d kept, %d dropped, snapshot matches"
                    % (which, len(audit["kept"]), len(audit["dropped"]))
                )
    say("build_public_seed --check: %s" % ("PASS" if ok else "FAIL"))
    return ok


# --------------------------------------------------------------------------- preflight


def refusals(porcelain, verdicts):
    """The preflight decision, as a pure function so --selftest can plant each input.

    `porcelain` is `git status --porcelain` output; `verdicts` maps a gate name to its exit code."""
    out = []
    if porcelain.strip():
        n = len([ln for ln in porcelain.splitlines() if ln.strip()])
        out.append(
            "the private tree is dirty (%d path(s) in `git status --porcelain`) -- the publish set "
            "is derived from git ls-files, so an uncommitted edit would ship content no commit "
            "describes" % n
        )
    for name, rc in sorted(verdicts.items()):
        if rc != 0:
            out.append(
                "%s did not PASS (exit %d) -- the cut is only as good as its ledger" % (name, rc)
            )
    return out


def preflight(say=print):
    porcelain = subprocess.run(
        ["git", "-C", REPO, "status", "--porcelain"], capture_output=True, text=True
    ).stdout
    verdicts = {}
    for name, argv in (
        (
            "check_publishable",
            [sys.executable, os.path.join(REPO, "tools", "check_publishable.py")],
        ),
        (
            "check_publishable --closure",
            [sys.executable, os.path.join(REPO, "tools", "check_publishable.py"), "--closure"],
        ),
        (
            "lint_machine_paths",
            [sys.executable, os.path.join(REPO, "tools", "lint_machine_paths.py")],
        ),
    ):
        r = subprocess.run(argv, capture_output=True, text=True, cwd=REPO)
        verdicts[name] = r.returncode
        say("  preflight %-28s %s" % (name, "PASS" if r.returncode == 0 else "FAIL"))
        if r.returncode != 0:
            for ln in (r.stdout or "").splitlines()[-12:]:
                say("      %s" % ln)
    bad = refusals(porcelain, verdicts)
    for b in bad:
        say("REFUSED: %s" % b)
    return not bad


# --------------------------------------------------------------------------- materialize


def exec_bits(root=REPO):
    """Paths git records as mode 100755. shutil.copy2 carries the filesystem mode, which on Windows
    is not the mode git tracks -- so the executable bit is restated from the index."""
    out = subprocess.run(
        ["git", "-C", root, "ls-files", "-s"], capture_output=True, text=True, check=True
    ).stdout
    ex = set()
    for line in out.splitlines():
        parts = line.split("\t", 1)
        if len(parts) == 2 and parts[0].split()[0] == "100755":
            ex.add(parts[1].strip().replace("\\", "/"))
    return ex


def materialize(dest, allow_no_identity=False, say=print, into_clone=False):
    """Copy exactly the publish set into `dest`, then install the derived VCS config.

    `into_clone`: `dest` is a clone whose worktree has been emptied (`git rm -r .`), so the only
    thing allowed to be there is `.git`. Anything else is still a refusal -- a follow-up must not
    layer the publish set over files it did not put there."""
    led = cp.load_ledger()
    tracked = cp.tracked_paths()
    take = cp.publish_list(tracked, led)
    ex = exec_bits()

    present = os.listdir(dest) if os.path.exists(dest) else []
    allowed = {".git"} if into_clone else set()
    if set(present) - allowed:
        say("REFUSED: %s exists and is not empty" % dest)
        return None
    copied, missing = 0, []
    for p in take:
        src = os.path.join(REPO, p.replace("/", os.sep))
        if not os.path.isfile(src):
            missing.append(p)
            continue
        dst = os.path.join(dest, p.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        if p in ex:
            os.chmod(dst, os.stat(dst).st_mode | 0o111)
        copied += 1
    if missing:
        say("REFUSED: %d published path(s) are not files in the working tree" % len(missing))
        for p in missing[:10]:
            say("    %s" % p)
        return None
    say("materialized %d file(s) (%d executable)" % (copied, len(ex & set(take))))

    for which, (text, audit) in derive_both(say=say).items():
        _write(os.path.join(dest, which), text)
        say(
            "  derived %s -- %d entries kept, %d dropped"
            % (which, len(audit["kept"]), len(audit["dropped"]))
        )

    if not verify_tree(dest, take, led, say=say):
        return None
    if not sweep(dest, take, allow_no_identity=allow_no_identity, say=say):
        return None
    return take


def verify_tree(dest, take, led, say=print):
    """Every file in `dest` is a published path, and every published path is in `dest`.

    The second half is the one that cannot be seen by inspection: a dropped file leaves nothing
    behind to notice."""
    have = set()
    for root, _dirs, files in os.walk(dest):
        if ".git" in root.replace("\\", "/").split("/"):
            continue
        for f in files:
            rel = os.path.relpath(os.path.join(root, f), dest).replace("\\", "/")
            have.add(rel)
    want = set(take) | set(SNAPSHOTS.values())
    smuggled = sorted(have - want)
    lost = sorted(want - have)
    ok = True
    if smuggled:
        ok = False
        say("FAIL: %d file(s) in the seed are not in the publish set:" % len(smuggled))
        _by, _r, _u = cp.classify(sorted(smuggled), led)
        for p in smuggled[:20]:
            rule = _by.get(p)
            say(
                "    %s  (%s)"
                % (p, "%s/%s" % (rule["id"], rule["disposition"]) if rule else "uncovered")
            )
    if lost:
        ok = False
        say("FAIL: %d published path(s) did not reach the seed:" % len(lost))
        for p in lost[:20]:
            say("    %s" % p)
    say("verify tree: %d file(s), %d smuggled, %d lost" % (len(have), len(smuggled), len(lost)))
    return ok


def sweep(dest, take, allow_no_identity=False, say=print):
    """Re-run the published-tier identity scan over the MATERIALIZED BYTES.

    Nothing here is a literal: the address classes and the user-profile shape come from
    lint_machine_paths, and the identity tokens are derived from `git config` and the gitignored
    machine-local config at run time. A scanner that embeds the secret it looks for publishes it."""
    import lint_machine_paths as lmp  # noqa: PLC0415

    scanlist = [p for p in lmp.published_paths() if p in set(take)]
    prefixes = lmp.default_path_prefixes()
    tokens = lmp.identity_tokens()
    vio = []
    for p in scanlist:
        full = os.path.join(dest, p.replace("/", os.sep))
        if not os.path.isfile(full):
            continue
        with open(full, "rb") as fh:
            blob = fh.read()
        v, _hits = lmp.scan_published(p, blob, prefixes, tokens, lmp.SITE_EXCEPTIONS)
        vio.extend(v)
    say(
        "secret sweep: %d file(s) scanned, %d identity token(s) derived, %d violation(s)"
        % (len(scanlist), len(tokens), len(vio))
    )
    for path, line, kind, tok in vio[:20]:
        say("    %s:%s  %s  %s" % (path, line, kind, tok))
    if not tokens and not allow_no_identity:
        # A ZERO-TOKEN SCAN IS A REFUSAL, not a note. The tokens come from `git config` and the
        # gitignored machine-local config, so a checkout that carries neither -- a bare clone, a
        # CI runner, a container -- derives nothing and the identity arm silently scans for the
        # empty set. That is the one failure mode where a green sweep and no sweep at all look
        # identical, which is precisely the shape this repo's lints refuse elsewhere.
        say(
            "REFUSED: zero identity tokens derived -- the identity arm would scan for nothing. "
            "Run from the tree whose git identity and machine-local config are configured, or "
            "pass MH_IDENTITY_TOKENS, or --allow-no-identity if that is genuinely the intent."
        )
        return False
    return not vio


# --------------------------------------------------------------------------- the seed repository


def _git(dest, *args, **kw):
    return subprocess.run(
        ["git", "-C", dest] + list(args), capture_output=True, text=True, check=False, **kw
    )


def ignored_published(dest, take, say=print):
    """Does the derived .gitignore swallow a published path? Asked of git, in the seed itself.

    NUL-SEPARATED, IN BYTES, ON PURPOSE. `text=True` here would translate every `\\n` in the input
    to `\\r\\n` on Windows, and git then treats the CR as part of the path -- which reports
    `mod_src/cfg/aliases.yaml` as ignored, because the trailing CR is exactly what stops the
    `!mod_src/cfg/aliases.yaml` negation from matching it. A false red on the one entry whose whole
    reason for existing is that negation."""
    r = subprocess.run(
        ["git", "-C", dest, "check-ignore", "-z", "--stdin"],
        input=b"\0".join(p.encode("utf-8") for p in take) + b"\0",
        capture_output=True,
    )
    hit = [
        p.decode("utf-8", "replace").replace("\\", "/") for p in r.stdout.split(b"\0") if p.strip()
    ]
    if hit:
        say("FAIL: the derived .gitignore ignores %d published path(s):" % len(hit))
        for p in hit[:20]:
            say("    %s" % p)
    return not hit


def seed(dest, take, branch=SEED_BRANCH, name=None, email=None, say=print):
    name = name or license_holder()
    email = author_email(email)
    if not name or not email:
        say(
            "REFUSED: the seed commit needs an author -- the NAME comes from LICENSE's "
            "copyright line (%r here) and the EMAIL from --author-email or "
            "MH_SEED_AUTHOR_EMAIL (%r here)." % (name, email)
        )
        return None
    r = _git(dest, "init", "-b", branch, "-q")
    if r.returncode:
        say("git init failed: %s" % (r.stderr or r.stdout).strip())
        return None
    _git(dest, "config", "user.name", name)
    _git(dest, "config", "user.email", email)
    # The private tree's blobs are LF and .gitattributes pins eol=lf, so `add` normalises; saying so
    # explicitly keeps ~1,700 informational CRLF warnings out of the run's output.
    _git(dest, "config", "core.autocrlf", "false")

    if not ignored_published(dest, take, say=say):
        return None

    r = _git(dest, "add", "-A")
    if r.returncode:
        say("git add failed: %s" % (r.stderr or r.stdout).strip())
        return None
    r = _git(dest, "commit", "-q", "-m", SEED_COMMIT_MESSAGE)
    if r.returncode:
        say("git commit failed: %s" % (r.stderr or r.stdout).strip())
        return None
    return _verify_commit(dest, take, branch, name, email, "SEED", say=say)


def _verify_commit(dest, take, branch, name, email, label, say=print, public_check=True):
    """After a commit in `dest`: the tracked set is exactly the publish set plus the derived
    config, and the tree passes check_publishable --public from its own copy of the tool. Shared by
    the seed and the follow-up so a follow-up cannot be held to a weaker standard than the seed."""
    tracked = [
        p.strip().replace("\\", "/")
        for p in _git(dest, "ls-files").stdout.splitlines()
        if p.strip()
    ]
    want = set(take) | set(SNAPSHOTS.values())
    if set(tracked) != want:
        say(
            "FAIL: the seed tracks %d path(s); the publish set plus the derived config is %d"
            % (len(tracked), len(want))
        )
        for p in sorted(want - set(tracked))[:20]:
            say("    missing: %s" % p)
        for p in sorted(set(tracked) - want)[:20]:
            say("    extra:   %s" % p)
        return None

    head = _git(dest, "rev-parse", "HEAD").stdout.strip()
    tree = _git(dest, "rev-parse", "HEAD^{tree}").stdout.strip()
    total = 0
    for root, dirs, files in os.walk(dest):
        if ".git" in root.replace("\\", "/").split("/"):
            continue
        for f in files:
            total += os.path.getsize(os.path.join(root, f))
    say("")
    say("%-5s branch %s  commit %s" % (label, branch, head[:12]))
    say("      tree   %s" % tree)
    say("      files  %d tracked" % len(tracked))
    say("      size   %d bytes (%.2f MiB) of worktree" % (total, total / 1048576.0))
    say("      author %s <%s>" % (name, email))

    if public_check:
        r = subprocess.run(
            [sys.executable, os.path.join(dest, "tools", "check_publishable.py"), "--public"],
            capture_output=True,
            text=True,
        )
        for ln in (r.stdout or "").splitlines()[-6:]:
            say("      %s" % ln)
        if r.returncode:
            say("FAIL: the tree does not satisfy check_publishable --public")
            return None
    return {"head": head, "tree": tree, "files": len(tracked), "bytes": total}


def follow_up(
    url,
    dest,
    branch=SEED_BRANCH,
    name=None,
    email=None,
    message=None,
    say=print,
    materialize_fn=None,
    public_check=True,
):
    """A FOLLOW-UP commit on an already-published repository (fork F5N, 2026-09-16).

    The seed is one commit; the public repo then lives on and every later private milestone lands
    on it as a new commit rather than a replaced history (the user's call at F5I). The shape is:
    clone `url` at `branch`, EMPTY the worktree (`git rm -r .`, so a file the publish set no longer
    carries is a deletion in the commit, not a survivor), materialize the publish set into the
    emptied clone, commit as the LICENSE holder, and hold the result to the seed's own standard
    (`_verify_commit`). An unchanged publish set is reported and NOT committed. Pushing is a
    separate, explicit step (`--push`), exactly as for the seed.

    `materialize_fn` / `public_check` exist for the selftest, which drives the git mechanics on a
    two-file synthetic set instead of the 2,000-file real one."""
    name = name or license_holder()
    email = author_email(email)
    if not name or not email:
        say("REFUSED: the follow-up commit needs an author (see --author-email)")
        return None
    if os.path.exists(dest) and os.listdir(dest):
        say("REFUSED: %s exists and is not empty" % dest)
        return None
    # `-c core.autocrlf=false` BEFORE the checkout: on a box whose global config says autocrlf=true
    # (every GitHub Windows runner), a fresh clone's worktree already differs from its index by
    # line endings, and `git rm -r .` below refuses with "local modifications". Setting the option
    # after the clone is too late -- the files are already checked out CRLF. Found by the public
    # CI's first run of this arm (2026-09-16); the tool's own machine never had the setting.
    r = _git(
        os.path.dirname(dest) or ".",
        "clone",
        "-q",
        "-c",
        "core.autocrlf=false",
        "--branch",
        branch,
        url,
        dest,
    )
    if r.returncode:
        say("git clone failed: %s" % (r.stderr or r.stdout).strip())
        return None
    _git(dest, "config", "user.name", name)
    _git(dest, "config", "user.email", email)
    _git(dest, "config", "core.autocrlf", "false")
    before = _git(dest, "rev-parse", "HEAD").stdout.strip()
    r = _git(dest, "rm", "-r", "-q", ".")
    if r.returncode:
        say("git rm failed: %s" % (r.stderr or r.stdout).strip())
        return None
    # `git rm` leaves the emptied directories behind; materialize's refusal is about FILES, but
    # clear them so a stray non-tracked file cannot hide in one either.
    for entry in os.listdir(dest):
        if entry != ".git":
            full = os.path.join(dest, entry)
            shutil.rmtree(full) if os.path.isdir(full) else os.remove(full)
    take = (materialize_fn or materialize)(dest, say=say, into_clone=True)
    if take is None:
        return None
    if not ignored_published(dest, take, say=say):
        return None
    r = _git(dest, "add", "-A")
    if r.returncode:
        say("git add failed: %s" % (r.stderr or r.stdout).strip())
        return None
    if _git(dest, "diff", "--cached", "--quiet").returncode == 0:
        say("NOTHING TO COMMIT: the public tree at %s already equals the publish set" % before[:12])
        return {"head": before, "unchanged": True, "files": len(take)}
    stat = _git(dest, "diff", "--cached", "--shortstat").stdout.strip()
    r = _git(dest, "commit", "-q", "-m", message or follow_up_message())
    if r.returncode:
        say("git commit failed: %s" % (r.stderr or r.stdout).strip())
        return None
    info = _verify_commit(
        dest, take, branch, name, email, "FUP", say=say, public_check=public_check
    )
    if info is None:
        return None
    say("      on top of %s: %s" % (before[:12], stat))
    info["parent"] = before
    info["unchanged"] = False
    return info


def follow_up_message():
    """The default follow-up subject names the PRIVATE commit it was cut from, which is the one
    link between a public commit and the private history behind it."""
    head = subprocess.run(
        ["git", "rev-parse", "--short=12", "HEAD"], cwd=REPO, capture_output=True, text=True
    ).stdout.strip()
    subject = subprocess.run(
        ["git", "log", "-1", "--format=%s"], cwd=REPO, capture_output=True, text=True
    ).stdout.strip()
    return "publish: private %s -- %s" % (head, subject[:200])


def push(dest, url, branch=SEED_BRANCH, say=print, existing_origin=False):
    """Only ever runs when --push is given. Point it at a throwaway bare repo first, and set that
    repo's HEAD to `branch` before cloning (`git init --bare` leaves HEAD on master).
    `existing_origin`: the follow-up clone already has origin = url; assert rather than add."""
    if existing_origin:
        have = _git(dest, "remote", "get-url", "origin").stdout.strip()
        if have != url:
            say("REFUSED: the clone's origin is %s, --push says %s" % (have, url))
            return False
    else:
        r = _git(dest, "remote", "add", "origin", url)
        if r.returncode:
            say("git remote add failed: %s" % (r.stderr or r.stdout).strip())
            return False
    r = _git(dest, "push", "-u", "origin", branch)
    say((r.stdout or "").strip() or (r.stderr or "").strip())
    return r.returncode == 0


# --------------------------------------------------------------------------- selftest


def selftest(say=print):
    results = []

    def arm(name, cond):
        results.append((name, bool(cond)))
        say("  %-68s %s" % (name, "ok" if cond else "FAILED"))

    # 1. The preflight refuses a dirty tree, and refuses a red gate. Planted as inputs to the pure
    #    decision function, so the arm does not need to dirty the real tree to prove it.
    arm("clean tree + green gates is not refused", refusals("", {"a": 0, "b": 0}) == [])
    arm("a dirty private tree is REFUSED", bool(refusals(" M an_edited_file\n", {"a": 0})))
    arm("a red check_publishable is REFUSED", bool(refusals("", {"check_publishable": 1})))
    arm("a red closure arm is REFUSED", bool(refusals("", {"check_publishable --closure": 1})))

    led = cp.load_ledger()
    tracked = cp.tracked_paths()

    # A SYNTHETIC LEDGER AND A SYNTHETIC TABLE FROM HERE DOWN. Every arm below has to fire in BOTH
    # trees, and the real ledger cannot carry them in the public one: there is no withheld path to
    # smuggle, and the checkout's .gitignore is already the DERIVED file, whose entry set no longer
    # matches the table. Planting against synthetic inputs keeps the arms answering the question
    # they are named after instead of passing because the tree made them vacuous.
    syn_led = {
        "rules": [
            {"id": "syn-withhold", "disposition": "withhold", "globs": ["tools/**"], "why": "x"},
            {"id": "syn-publish", "disposition": "publish", "globs": ["**"], "why": "x"},
        ]
    }
    for r in syn_led["rules"]:
        r["_res"] = [cp.glob_re(g) for g in r["globs"]]
    syn_groups = [("junk", "junk")]
    syn_src = "# a private config\nbuild_out/\nprivate_only/\n"
    syn_spec = (
        [
            _keep("build_out/", "junk", "a build output the public tree produces"),
            _drop("private_only/", "a private layer", withheld_by="syn-withhold"),
        ],
        "# derived\n",
        syn_groups,
        None,
    )

    # 2. A withheld path smuggled into the out dir is caught by the post-materialize tree check.
    tmp = tempfile.mkdtemp(prefix="f5m_seed_")
    try:
        take = ["tools/check_publishable.py"]
        for p in take:
            d = os.path.join(tmp, p.replace("/", os.sep))
            os.makedirs(os.path.dirname(d), exist_ok=True)
            with open(d, "w", encoding="utf-8") as fh:
                fh.write("x\n")
        for which in SNAPSHOTS.values():
            _write(os.path.join(tmp, which), "x\n")
        arm("a faithful tree verifies", verify_tree(tmp, take, syn_led, say=lambda *_a: None))
        smug = "private_only/a_withheld_file"
        d = os.path.join(tmp, smug.replace("/", os.sep))
        os.makedirs(os.path.dirname(d), exist_ok=True)
        with open(d, "w", encoding="utf-8") as fh:
            fh.write("leak\n")
        arm(
            "a withheld file smuggled into the out dir goes RED",
            not verify_tree(tmp, take, syn_led, say=lambda *_a: None),
        )
        os.remove(d)
        os.remove(os.path.join(tmp, take[0].replace("/", os.sep)))
        arm(
            "a published file MISSING from the out dir goes RED",
            not verify_tree(tmp, take, syn_led, say=lambda *_a: None),
        )
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # 3. The derivation's own negatives, against the synthetic table.
    def _derive(src, spec):
        return derive(".gitignore", src, syn_led, tracked, spec=spec, say=lambda *_a: None)

    def refuses(src, spec):
        try:
            _derive(src, spec)
            return False
        except SystemExit:
            return True

    text, audit = _derive(syn_src, syn_spec)
    arm(
        "a faithful derivation keeps the public entry and drops the private one",
        "build_out/" in text and "private_only/" not in text and len(audit["dropped"]) == 1,
    )
    arm(
        "an undispositioned .gitignore entry stops the build",
        refuses(syn_src + "new_rule_nobody_dispositioned/\n", syn_spec),
    )
    arm(
        "a table row whose entry the file no longer carries stops the build",
        refuses("build_out/\n", syn_spec),
    )
    keep_withheld = (
        [
            _keep("build_out/", "junk", "a build output"),
            dict(_keep("private_only/", "junk", "kept anyway"), withheld_by="syn-withhold"),
        ],
    ) + syn_spec[1:]
    arm(
        "an entry whose subject is WITHHELD cannot survive derivation",
        refuses(syn_src, keep_withheld),
    )
    lean_on_publish = (
        [
            _keep("build_out/", "junk", "a build output"),
            _drop("private_only/", "a private layer", withheld_by="syn-publish"),
        ],
    ) + syn_spec[1:]
    arm("a drop row leaning on a PUBLISH rule goes RED", refuses(syn_src, lean_on_publish))
    lean_on_dead = (
        [
            _keep("build_out/", "junk", "a build output"),
            _drop("private_only/", "a private layer", withheld_by="no-such-rule"),
        ],
    ) + syn_spec[1:]
    arm("a drop row naming a rule that does not exist goes RED", refuses(syn_src, lean_on_dead))
    false_no_producer = (
        [
            _keep("build_out/", "junk", "a build output"),
            _drop("tools/", "nothing writes it", no_producer="git tracks nothing under tools/"),
        ],
    ) + syn_spec[1:]
    arm(
        "a no_producer claim contradicted by git ls-files goes RED",
        refuses("build_out/\ntools/\n", false_no_producer),
    )

    # 4. An ignore rule that swallows a published path is caught by git itself.
    tmp = tempfile.mkdtemp(prefix="f5m_ign_")
    try:
        subprocess.run(["git", "-C", tmp, "init", "-q"], check=True)
        _write(os.path.join(tmp, ".gitignore"), "tmp/\n")
        arm(
            "a sane ignore file swallows no published path",
            ignored_published(tmp, ["tools/check_publishable.py"], say=lambda *_a: None),
        )
        _write(os.path.join(tmp, ".gitignore"), "tmp/\ntools/\n")
        arm(
            "an ignore rule swallowing a published path goes RED",
            not ignored_published(tmp, ["tools/check_publishable.py"], say=lambda *_a: None),
        )
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # 5. The derivation reproduces the committed snapshots (the same question --check asks, so a
    #    --selftest run in either tree still exercises it).
    arm("the committed snapshots reproduce", check_config(say=lambda *_a: None))

    # 6. The follow-up's git mechanics, on a synthetic two-file publish set against a throwaway
    #    bare repo: a changed file is a modification, a dropped file is a DELETION (not a survivor),
    #    a new file is added, the tracked set is exactly the set, and an unchanged set commits
    #    nothing. The real materialize and the real public check are injected out -- they are the
    #    seed's arms, proven above and by --check; this arm is about the clone/strip/commit shape.
    tmp = tempfile.mkdtemp(prefix="f5n_fup_")
    try:
        bare = os.path.join(tmp, "remote.git")
        subprocess.run(["git", "init", "-q", "--bare", "-b", SEED_BRANCH, bare], check=True)
        first = os.path.join(tmp, "first")
        os.makedirs(first)
        for rel, text in (
            ("a.txt", "a1\n"),
            ("b.txt", "b\n"),
            (".gitignore", "x\n"),
            (".gitattributes", "y\n"),
        ):
            _write(os.path.join(first, rel), text)
        _git(first, "init", "-q", "-b", SEED_BRANCH)
        _git(first, "config", "user.name", "t")
        _git(first, "config", "user.email", "t@t")
        _git(first, "add", "-A")
        _git(first, "commit", "-q", "-m", "seed")
        _git(first, "remote", "add", "origin", bare)
        _git(first, "push", "-q", "-u", "origin", SEED_BRANCH)

        def syn_materialize(
            dest, say=print, into_clone=False, files=(("a.txt", "a2\n"), ("c.txt", "c\n"))
        ):
            if set(os.listdir(dest)) - {".git"}:
                return None
            for rel, text in files:
                _write(os.path.join(dest, rel), text)
            for which in SNAPSHOTS.values():
                _write(os.path.join(dest, which), "z\n")
            return [rel for rel, _t in files]

        said = []

        def quiet(*a):
            said.append(" ".join(str(x) for x in a))

        c1 = os.path.join(tmp, "c1")
        info = follow_up(
            bare,
            c1,
            name="t",
            email="t@t",
            message="fup",
            say=quiet,
            materialize_fn=syn_materialize,
            public_check=False,
        )
        tracked = set(_git(c1, "ls-files").stdout.split())
        if not (info and not info.get("unchanged")):
            for ln in said[-6:]:
                say("      | %s" % ln)
        arm(
            "follow-up: a commit lands on top of the seed", bool(info) and not info.get("unchanged")
        )
        arm(
            "follow-up: tracked set == the new publish set (b.txt DELETED, c.txt added)",
            tracked == {"a.txt", "c.txt", ".gitignore", ".gitattributes"},
        )
        arm(
            "follow-up: the seed is the parent",
            bool(info) and info.get("parent") == _git(c1, "rev-parse", "HEAD~1").stdout.strip(),
        )
        arm(
            "follow-up: push refuses a URL that is not the clone's origin",
            not push(c1, bare + ".elsewhere", say=quiet, existing_origin=True),
        )
        arm(
            "follow-up: push to the clone's own origin succeeds",
            push(c1, bare, say=quiet, existing_origin=True),
        )
        c2 = os.path.join(tmp, "c2")
        info2 = follow_up(
            bare,
            c2,
            name="t",
            email="t@t",
            message="fup2",
            say=quiet,
            materialize_fn=syn_materialize,
            public_check=False,
        )
        arm(
            "follow-up: an unchanged publish set commits NOTHING",
            bool(info2)
            and info2.get("unchanged") is True
            and _git(c2, "rev-list", "--count", "HEAD").stdout.strip() == "2",
        )
        c3 = os.path.join(tmp, "c3")
        os.makedirs(c3)
        _write(os.path.join(c3, "stray"), "x\n")
        arm(
            "follow-up: a non-empty clone dir is REFUSED",
            follow_up(
                bare,
                c3,
                name="t",
                email="t@t",
                say=quiet,
                materialize_fn=syn_materialize,
                public_check=False,
            )
            is None,
        )
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    bad = [n for n, okk in results if not okk]
    say("")
    say("build_public_seed --selftest: %d arm(s), %d failed" % (len(results), len(bad)))
    return not bad


# --------------------------------------------------------------------------- main


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", help="materialize the publish set into this directory")
    ap.add_argument("--seed", help="materialize, then git init + one authored commit")
    ap.add_argument(
        "--push",
        help="with --seed: git remote add origin <url> && git push -u; with --follow-up: push "
        "the follow-up commit (must equal the --follow-up url)",
    )
    ap.add_argument(
        "--follow-up",
        metavar="URL",
        help="clone URL, replace its tree with the publish set, ONE follow-up commit (fork F5N). "
        "Needs --clone-dir.",
    )
    ap.add_argument("--clone-dir", help="with --follow-up: where to clone (must not exist)")
    ap.add_argument(
        "--message",
        help="with --follow-up: the commit message (default: names the private HEAD it was cut from)",
    )
    ap.add_argument(
        "--author-email",
        help="the seed commit author's email (or MH_SEED_AUTHOR_EMAIL). The NAME is read "
        "from LICENSE's copyright line, so the commit and the copyright cannot disagree.",
    )
    ap.add_argument("--branch", default=SEED_BRANCH, help="seed branch name (default: main)")
    ap.add_argument("--check", action="store_true", help="the derivation drift gate")
    ap.add_argument("--update-config", action="store_true", help="rewrite the committed snapshots")
    ap.add_argument("--selftest", action="store_true", help="the planted negatives")
    ap.add_argument(
        "--allow-no-identity",
        action="store_true",
        help="proceed even though no identity token could be derived (the sweep's identity arm "
        "then scans for nothing -- say so deliberately)",
    )
    ap.add_argument("--json", action="store_true", help="with --seed: emit the metrics as JSON")
    args = ap.parse_args()

    if args.selftest:
        return 0 if selftest() else 1
    if args.check:
        return 0 if check_config() else 1
    if args.update_config:
        for which, (text, audit) in derive_both().items():
            _write(snapshot_path(which), text)
            print(
                "wrote %s (%d kept, %d dropped)"
                % (
                    os.path.relpath(snapshot_path(which), REPO).replace("\\", "/"),
                    len(audit["kept"]),
                    len(audit["dropped"]),
                )
            )
        return 0

    if args.follow_up:
        if args.seed or args.out:
            ap.error("--follow-up excludes --seed/--out")
        if not args.clone_dir:
            ap.error("--follow-up needs --clone-dir")
        if args.push and args.push != args.follow_up:
            ap.error(
                "--push must equal the --follow-up url (a follow-up goes back where it came from)"
            )
        if not (license_holder() and author_email(args.author_email)):
            print("REFUSED: the follow-up commit needs an author (LICENSE holder + --author-email)")
            return 1
        if not preflight():
            return 1
        dest = os.path.abspath(args.clone_dir)
        info = follow_up(
            args.follow_up,
            dest,
            branch=args.branch,
            email=args.author_email,
            message=args.message,
        )
        if info is None:
            return 1
        if args.json:
            print(json.dumps(info, indent=1))
        if info.get("unchanged"):
            return 0
        if args.push:
            return 0 if push(dest, args.push, branch=args.branch, existing_origin=True) else 1
        return 0

    dest = args.seed or args.out
    if not dest:
        ap.error(
            "one of --out, --seed, --follow-up, --check, --update-config, --selftest is required"
        )
    if args.push and not args.seed:
        ap.error("--push needs --seed (or --follow-up)")
    dest = os.path.abspath(dest)

    # The author is resolved BEFORE the copy: a missing email should cost a second, not a 2,370-file
    # materialize followed by a refusal.
    if args.seed and not (license_holder() and author_email(args.author_email)):
        print(
            "REFUSED: the seed commit needs an author -- the NAME comes from LICENSE's copyright "
            "line (%r here) and the EMAIL from --author-email or MH_SEED_AUTHOR_EMAIL (%r here)."
            % (license_holder(), author_email(args.author_email))
        )
        return 1
    if not preflight():
        return 1
    take = materialize(dest, allow_no_identity=args.allow_no_identity)
    if take is None:
        return 1
    if not args.seed:
        print("out tree ready: %s" % dest)
        return 0

    info = seed(dest, take, branch=args.branch, email=args.author_email)
    if info is None:
        return 1
    if args.json:
        print(json.dumps(info, indent=1))
    if args.push:
        return 0 if push(dest, args.push, branch=args.branch) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
