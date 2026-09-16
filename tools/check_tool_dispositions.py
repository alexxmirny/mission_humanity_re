#!/usr/bin/env python3
"""check_tool_dispositions.py -- every top-level tools/*.py carries a fork disposition, and every
KEEP row can name what runs it.

WHAT THIS REPLACES. F0 triaged `tools/` into KEEP / ARCHIVE / UNSURE as three markdown tables in
The fork inventory section 1. Those tables recorded a real measurement and they stay there as
the F0 record -- but prose cannot be re-run, so within a month the tree had moved underneath them:
12 scripts had been DELETED (the F2D shadow-oracle purge) and 20 had ARRIVED, none of which any
table knew about, and one row (gen_libmh_rebind.py) said ARCHIVE about a tool `lint_repo.py` gates
on every run. A triage nobody can re-run is a triage that is wrong and cannot say so.

So the live source of truth is `tools/data/tool_dispositions.json` -- one row per top-level
tools/*.py -- and this file is the gate that keeps it true. The failure it stops is the ordinary
edit: ADDING or DELETING a tool. A new script cannot arrive already assumed publishable, and a
deleted one cannot leave its row behind to be read years later as a live classification.

WHAT THE TWO CLASSES MEAN. `class` is a FORK disposition, not a liveness claim:

  keep    -- the tool belongs to machinery the fork carries: the gate, lint, the build, the
             harness, libmh/mh_net, or an operator's hands.
  archive -- the tool belongs to machinery the fork DROPS (the Ghidra DB pipeline, the naming
             pipeline, the migration/promotion/ledger machinery, the research-doc lints).

An `archive` row may still be wired at HEAD, and many are: HEAD is pre-fork. `--wired` prints them,
because each one is a `lint_repo.py`/`run_gate.py` row that has to come out WITH its tool -- that
list is a fork-cut worklist, and deriving it beats remembering it.

THE ASSERTIONS (--check, the lint_repo row):

  a) every top-level tools/*.py has a row                 -- a tool cannot arrive unclassified
  b) every row names a tool that exists                   -- a dead rule cannot outlive its file
  c) every keep row without `hand_run` is reachable from
     a MACHINE invoker                                    -- a keep claim names what runs it
  d) no `hand_run` row HAS a machine invoker              -- the marker cannot become a blanket
                                                             exemption somebody pastes forward

A MACHINE INVOKER is something that runs the tool with no human in the loop: a `lint_repo.py` row,
a `run_gate.py` unit, a `.vcxproj` custom-build step, or a `.claude/workflows/*.js` script. Python
invokers are matched on the QUOTED file name (`"foo.py"`), which is how every row in lint_repo and
run_gate is actually built -- prose that names a tool in a comment does not match. The other three
are matched on the PATH form (`tools/foo.py`), i.e. a command line rather than a mention.  # CITATION-OK

Reachability then runs through the keep set itself: a keep tool that another keep tool executes
(quoted name) or imports (module name -- `import foo` / `from foo import`, never a path grep) is
reachable. Roots are the machine-invoked rows plus the declared `hand_run` ones, so a hand-run
tool's private helper is covered without needing its own marker, and a mutually-referencing island
of keep rows that nothing enters is NOT.

A SKILL IS NOT A MACHINE INVOKER. `.claude/skills/*/*.md` is a runbook a human follows, so a tool
whose only citation is a skill is a HAND-RUN tool and must say so. The skills are still searched --
`--evidence` quotes them as corroboration for a hand_run row -- they just do not satisfy (c).
This is the mp_run.py precedent: uninvoked is not the same as unused.

WHAT THIS DELIBERATELY DOES NOT CHECK: tools/data reachability. The obvious sibling rule -- "every
tools/data/*.json has a reader" -- would be DISHONEST here. SEVEN of the eight `tools/data/sim_resid_*`
files have no literal reader anywhere in tools/*.py (measured 2026-09-15; only sim_resid_migration.json
is ever spelled out), and every one of them is read, because the path is BUILT from the domain name:
coverage.py:316 `"%s_coverage.json" % domain`, coverage.py:1648 a `*_coverage.json` glob,
check_state_bindings.py:168 `"%s_migration.json" % domain`. A literal-grep reachability lint over
that directory therefore reports seven orphans that are not orphans, and the only way to make it
green is to delete live data. tools/data/publish_ledger.json owns the data side by DISPOSITION
instead, which needs no reachability claim.

usage:
  python tools/check_tool_dispositions.py            # the gate (the lint_repo row)
  python tools/check_tool_dispositions.py --wired    # archive rows HEAD still wires (fork worklist)
  python tools/check_tool_dispositions.py --evidence # per-tool evidence, as measured right now
  python tools/check_tool_dispositions.py --ledger   # the publish ledger's cut == the archive class
  python tools/check_tool_dispositions.py --selftest # the gate's own negative cases
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
DISPOSITIONS = "tools/data/tool_dispositions.json"
LEDGER = "tools/data/publish_ledger.json"
ARCHIVE_RULE = "archive-class-tools"
TOOLS_TREE_RULE = "tools-tree"

META_TEXT = (
    "Fork disposition for every TOP-LEVEL tools/*.py. Generated-and-gated data, not prose: "
    "tools/check_tool_dispositions.py is the lint_repo row that keeps it true, and "
    "the F0 fork inventory's section 1 is the measurement this was seeded from. "
    "class=keep means the tool belongs to machinery the fork carries (gate, lint, build, harness, "
    "libmh/mh_net, or an operator's hands); class=archive means it belongs to machinery the fork "
    "drops, WHICH IS NOT A LIVENESS CLAIM -- many archive rows are still wired at HEAD "
    "(`--wired` prints them; each is an invoker the fork cut must remove too). "
    "hand_run=true declares 'no machine invoker, a human runs this' and is checked both ways."
)

SKILL_DIR = os.path.join(".claude", "skills")


def _read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def tool_names(root):
    """The top-level tools/*.py roster: TRACKED or merely PRESENT, unioned.

    Tracked is the roster that matters for the fork, but present is what makes the rule provable:
    a new tool sitting in the work tree unstaged is still a tool, and a gate that only sees the
    index can only be red-proofed by editing the index. One row is a cheap price for a script
    somebody is still writing -- tools/oneoff/ and tmp/ are where a genuinely disposable one goes.
    A planted selftest tree has no git at all and reaches the same answer by listing."""
    names = set()
    tools_dir = os.path.join(root, "tools")
    if os.path.isdir(tools_dir):
        names |= {f for f in os.listdir(tools_dir) if f.endswith(".py")}
    try:
        out = subprocess.check_output(
            ["git", "-C", root, "ls-files", "tools/*.py"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
        names |= {p.split("/")[-1] for p in out.split() if p.count("/") == 1}
    except (OSError, subprocess.CalledProcessError):
        pass
    return sorted(names)


def _invoker_files(root):
    files = [("lint", os.path.join(root, "tools", "lint_repo.py"))]
    files.append(("gate", os.path.join(root, "tools", "run_gate.py")))
    for dirpath, dirs, names in os.walk(os.path.join(root, "src")):
        dirs[:] = [d for d in dirs if d not in (".git", "build", "x64", "Debug", "Release")]
        for n in names:
            if n.endswith(".vcxproj"):
                files.append(("vcxproj", os.path.join(dirpath, n)))
    wf = os.path.join(root, ".claude", "workflows")
    if os.path.isdir(wf):
        for n in sorted(os.listdir(wf)):
            if n.endswith(".js"):
                files.append(("workflow", os.path.join(wf, n)))
    return [(k, p) for k, p in files if os.path.isfile(p)]


def _quoted(name):
    return re.compile(r"""["']%s["']""" % re.escape(name))


def _path_form(name):
    return re.compile(r"tools[/\\]%s" % re.escape(name))


def machine_invokers(root, tools):
    """tool -> [repo-relative invoker paths]. Only machine invokers; skills are not one."""
    hits = {}
    for kind, path in _invoker_files(root):
        txt = _read(path)
        rel = os.path.relpath(path, root).replace("\\", "/")
        for t in tools:
            rx = _quoted(t) if path.endswith(".py") else _path_form(t)
            if rx.search(txt):
                hits.setdefault(t, []).append(rel)
    return {t: sorted(set(v)) for t, v in hits.items()}


def skill_mentions(root, tools):
    """tool -> [skill doc paths] that spell a `tools/foo.py` command. Corroboration for a  # CITATION-OK
    hand_run row; NOT an invoker."""
    hits = {}
    base = os.path.join(root, SKILL_DIR)
    if not os.path.isdir(base):
        return hits
    for dirpath, _dirs, names in os.walk(base):
        for n in names:
            if not n.endswith(".md"):
                continue
            path = os.path.join(dirpath, n)
            txt = _read(path)
            rel = os.path.relpath(path, root).replace("\\", "/")
            for t in tools:
                if _path_form(t).search(txt):
                    hits.setdefault(t, []).append(rel)
    return {t: sorted(set(v)) for t, v in hits.items()}


def tool_refs(root, tools):
    """tool -> set(tools it executes or imports). Execution is the quoted file name; import is the
    MODULE name, anchored at the start of a line -- not a path grep, which would count every
    docstring that happens to mention a sibling."""
    out = {}
    for t in tools:
        # SELF is excluded, for the reason every zero-survivor gate in this tree excludes itself:
        # the selftest fixtures below quote `lint_repo.py` and `run_gate.py` to shape a planted
        # tree, and read as edges those two literals would silently "invoke" the gate and the lint
        # -- run_gate.py in particular is genuinely HAND-RUN, and this file would be the only
        # reason its marker looked wrong. This checker executes no other tool, so nothing real is
        # lost by the exclusion.
        if t == os.path.basename(__file__):
            out[t] = set()
            continue
        txt = _read(os.path.join(root, "tools", t))
        seen = set()
        for u in tools:
            if u == t:
                continue
            stem = u[:-3]
            if _quoted(u).search(txt) or re.search(
                r"^[ \t]*(?:from[ \t]+%s[ \t]+import|import[ \t]+%s)\b" % (stem, stem),
                txt,
                re.M,
            ):
                seen.add(u)
        out[t] = seen
    return out


def reachable(keep, invokers, refs, hand_run):
    """Keep rows reachable from a machine invoker or a declared hand-run root."""
    front = {t for t in keep if t in invokers} | (set(hand_run) & keep)
    seen = set(front)
    while front:
        t = front.pop()
        for u in refs.get(t, ()):
            if u in keep and u not in seen:
                seen.add(u)
                front.add(u)
    return seen


def load_rows(root):
    path = os.path.join(root, DISPOSITIONS)
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)["rows"]


def _is_the_cut(root):
    """Is this checkout the PUBLIC CUT rather than the private working tree? (fork F5M S4b)

    Asked of the publish ledger, which declares the answer in its `_public_tree_marker` block, so
    that exactly one file decides it for every tool. Needed here because the archive cut made
    assertion (b) TREE-DEPENDENT: on the cut's output the 80 archive-class tools are withheld by
    design, so every one of their rows names a file that is not there -- which is the dead-rule
    failure (b) exists to catch, fired at the one tree where it is the intended state. Anything
    unreadable answers `private`, keeping the stricter arm rather than silently relaxing."""
    try:
        sys.path.insert(0, os.path.join(root, "tools"))
        import check_publishable as _cp

        # The import may be satisfied from ANOTHER tree's tools/ (sys.path carries this run's own
        # repo, and a planted selftest tree has no such module at all). Only trust the answer when
        # the module that answered belongs to the tree being asked about -- otherwise say `private`,
        # which is the strict arm and cannot turn a real dead row into a silent pass.
        if os.path.normcase(os.path.abspath(_cp.REPO)) != os.path.normcase(os.path.abspath(root)):
            return False
        return _cp.is_public_tree(_cp.tracked_paths(), _cp.load_ledger())
    except Exception:
        return False


def check(root, verbose=True):
    """The assertions. Returns a list of complaints (empty == green)."""
    bad = []
    tools = tool_names(root)
    if not tools:
        # A walker that visited nothing is not evidence of anything (G106).
        return ["found NO top-level tools/*.py under %s -- the scan is not evidence" % root]
    try:
        rows = load_rows(root)
    except (OSError, ValueError, KeyError) as exc:
        return ["%s is unreadable (%s) -- the gate cannot run" % (DISPOSITIONS, exc)]

    known = set(tools)
    # (a) every tool has a row.
    for t in tools:
        if t not in rows:
            bad.append(
                "tools/%s has NO disposition row -- add one to %s (class keep|archive, why, "
                "evidence)" % (t, DISPOSITIONS)
            )
    # (b) no row outlives its file -- EXCEPT on the cut's output, where an archive row's file is
    # absent BY DESIGN (the ledger's archive-class-tools rule withholds it). A keep row's file is
    # still required there: that is the public tree missing something it claims to carry.
    cut = _is_the_cut(root)
    for t in sorted(rows):
        if t in known:
            continue
        if cut and rows[t].get("class") == "archive":
            continue
        bad.append(
            "%s has a row for tools/%s, which does not exist -- delete the row" % (DISPOSITIONS, t)
        )

    live = {t: rows[t] for t in rows if t in known}
    for t in sorted(live):
        cls = live[t].get("class")
        if cls not in ("keep", "archive"):
            bad.append("tools/%s: class %r is neither keep nor archive" % (t, cls))
        if not live[t].get("why"):
            bad.append("tools/%s: no `why`" % t)
        if not live[t].get("evidence"):
            bad.append("tools/%s: no `evidence`" % t)

    keep = {t for t in live if live[t].get("class") == "keep"}
    hand = {t for t in live if live[t].get("hand_run")}
    invokers = machine_invokers(root, tools)
    refs = tool_refs(root, tools)

    # (d) the hand_run marker means what it says, so it cannot be pasted onto a gated tool.
    for t in sorted(hand):
        if t in invokers:
            bad.append(
                "tools/%s is marked hand_run, but %s invokes it -- drop the marker"
                % (t, ", ".join(invokers[t]))
            )
        if live[t].get("class") != "keep":
            bad.append(
                "tools/%s is marked hand_run but its class is %r" % (t, live[t].get("class"))
            )

    # (c) every keep row is reachable.
    reach = reachable(keep, invokers, refs, hand)
    for t in sorted(keep - reach):
        bad.append(
            "tools/%s is KEEP but nothing runs it: no lint_repo row, no run_gate unit, no vcxproj "
            "step, no workflow, and no keep tool executes or imports it. Either name the invoker "
            "or mark it hand_run: true." % t
        )

    if verbose:
        arch = len(live) - len(keep)
        if cut:
            print(
                "public cut: %d of %d rows name a tool this tree carries; the %d archive-class "
                "rows are withheld by the ledger and their absence is the intended state"
                % (len(live), len(rows), len(rows) - len(live))
            )
        print(
            "%d top-level tools: %d keep (%d machine-invoked, %d hand-run, %d reached through a "
            "keep tool), %d archive"
            % (
                len(tools),
                len(keep),
                len(keep & set(invokers)),
                len(hand),
                len(reach) - len(keep & set(invokers)) - len(hand - set(invokers)),
                arch,
            )
        )
    return bad


def check_ledger(root, verbose=True):
    """The publish ledger's archive-class WITHHOLD rule equals the archive class -- both directions.

    WHY THIS EXISTS (fork F5M S4b). The user ruled that the archive-class tools are withheld from
    the public tree rather than deleted from the repo, and that ruling is executed as one ledger row
    (`archive-class-tools`) whose globs enumerate them. An ENUMERATION IS A COPY, and a copy rots:
    the tool that decides the class lives in one file, the cut that acts on it in another, and
    nothing would notice them disagreeing. Both directions are failures and they fail differently:

      class archive, not in the rule  -- a research tool SHIPS in the public seed. Silent: it just
                                        sits there, and the first sign is a lint row nobody can run.
      in the rule, class keep         -- a tool the public tree's own lint imports is CUT OUT of it.
                                        This one is not silent, it is a red public CI, and it is the
                                        failure the S4a re-triage found eleven live instances of.

    The rule must also sit BEFORE the `tools-tree` publish row, because the ledger is first-match --
    behind it the carve-out matches nothing and every archive tool publishes again.
    """
    bad = []
    try:
        rows = load_rows(root)
    except (OSError, ValueError, KeyError) as exc:
        return ["%s is unreadable (%s) -- the ledger arm cannot run" % (DISPOSITIONS, exc)]
    try:
        with open(os.path.join(root, LEDGER), encoding="utf-8") as fh:
            rules = json.load(fh)["rules"]
    except (OSError, ValueError, KeyError) as exc:
        return ["%s is unreadable (%s) -- the ledger arm cannot run" % (LEDGER, exc)]

    ids = [r.get("id") for r in rules]
    if ARCHIVE_RULE not in ids:
        return [
            "%s has no `%s` rule -- the archive class is not withheld from the public tree at all"
            % (LEDGER, ARCHIVE_RULE)
        ]
    rule = rules[ids.index(ARCHIVE_RULE)]
    if rule.get("disposition") != "withhold":
        bad.append(
            "%s: rule `%s` has disposition %r, not `withhold`"
            % (LEDGER, ARCHIVE_RULE, rule.get("disposition"))
        )
    if TOOLS_TREE_RULE in ids and ids.index(ARCHIVE_RULE) > ids.index(TOOLS_TREE_RULE):
        bad.append(
            "%s: rule `%s` sits AFTER `%s`. The ledger is first-match, so behind the publish row it "
            "matches nothing and every archive tool publishes again -- move it in front."
            % (LEDGER, ARCHIVE_RULE, TOOLS_TREE_RULE)
        )

    declared = set(rule.get("globs", []))
    archive = {"tools/%s" % t for t in rows if rows[t].get("class") == "archive"}
    for g in sorted(archive - declared):
        bad.append(
            "%s is class ARCHIVE but `%s` does not withhold it -- it would SHIP in the public seed. "
            "Add the glob, or re-class the tool keep and say why." % (g, ARCHIVE_RULE)
        )
    for g in sorted(declared - archive):
        name = g.split("/")[-1]
        cls = rows.get(name, {}).get("class", "(no row)")
        bad.append(
            "`%s` withholds %s, whose disposition class is %s -- the public tree would be cut off "
            "from a tool it keeps. Drop the glob, or re-class the tool archive."
            % (ARCHIVE_RULE, g, cls)
        )
    if verbose and not bad:
        print(
            "the `%s` ledger rule withholds exactly the %d archive-class tools"
            % (ARCHIVE_RULE, len(archive))
        )
    return bad


def wired(root):
    """Archive rows HEAD still wires: the fork cut must remove these invokers too."""
    tools = tool_names(root)
    rows = load_rows(root)
    invokers = machine_invokers(root, tools)
    out = []
    for t in sorted(tools):
        r = rows.get(t)
        if r and r.get("class") == "archive" and t in invokers:
            out.append((t, invokers[t]))
    return out


def evidence(root):
    tools = tool_names(root)
    invokers = machine_invokers(root, tools)
    skills = skill_mentions(root, tools)
    refs = tool_refs(root, tools)
    rows = load_rows(root)
    for t in tools:
        r = rows.get(t, {})
        parents = sorted(
            u for u in tools if t in refs.get(u, ()) and rows.get(u, {}).get("class") == "keep"
        )
        print(
            "%-34s %-8s %s"
            % (
                t,
                r.get("class", "?") + ("*" if r.get("hand_run") else ""),
                "; ".join(
                    filter(
                        None,
                        [
                            "invoker: " + ", ".join(invokers[t]) if t in invokers else "",
                            "used by: " + ", ".join(parents) if parents else "",
                            "skill: " + ", ".join(skills[t]) if t in skills else "",
                        ],
                    )
                )
                or "(nothing)",
            )
        )


# ---------------------------------------------------------------------------
# --selftest: each refusal, fired at a planted tree through this same code.


def _plant(tmp, tools, rows, lint_body=None, gate_body=None, extra=None, ledger=None):
    os.makedirs(os.path.join(tmp, "tools", "data"))
    for name, body in tools.items():
        with open(os.path.join(tmp, "tools", name), "w", encoding="utf-8") as fh:
            fh.write(body)
    if lint_body is not None:
        with open(os.path.join(tmp, "tools", "lint_repo.py"), "w", encoding="utf-8") as fh:
            fh.write(lint_body)
    if gate_body is not None:
        with open(os.path.join(tmp, "tools", "run_gate.py"), "w", encoding="utf-8") as fh:
            fh.write(gate_body)
    for rel, body in (extra or {}).items():
        path = os.path.join(tmp, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(body)
    with open(os.path.join(tmp, DISPOSITIONS), "w", encoding="utf-8") as fh:
        json.dump({"_meta": "selftest", "rows": rows}, fh)
    if ledger is not None:
        with open(os.path.join(tmp, LEDGER), "w", encoding="utf-8") as fh:
            json.dump(ledger, fh)


KEEP_ROW = {"class": "keep", "why": "w", "evidence": "e"}
ARCH_ROW = {"class": "archive", "why": "w", "evidence": "e"}
# Shaped exactly like the real rows, because the match is the QUOTED FILE NAME and a path-form
# string would not fire -- if these two literals stop resembling lint_repo.py / run_gate.py, the
# selftest stops testing the rule the gate actually applies.
LINT_OK = 'check("x", [sys.executable, os.path.join(REPO, "tools", "alpha.py")])\n'
GATE_OK = (
    'unit("y", [sys.executable, os.path.join(REPO, "tools", "beta.py")])\n'
    'unit("lint", [sys.executable, os.path.join(REPO, "tools", "lint_repo.py")])\n'
)


def selftest():
    """The gate's own negative cases. A checker nobody has seen fail is a checker nobody tested."""
    base_tools = {"alpha.py": "# alpha\n", "beta.py": "# beta\n"}
    base_rows = {
        "alpha.py": dict(KEEP_ROW),
        "beta.py": dict(KEEP_ROW),
        "lint_repo.py": dict(KEEP_ROW),
        "run_gate.py": dict(KEEP_ROW, hand_run=True),
    }
    cases = []

    # (a) a tool nobody dispositioned.
    t = dict(base_tools, gamma="")
    t["gamma.py"] = "# gamma\n"
    del t["gamma"]
    cases.append(("(a) an undispositioned tool", t, dict(base_rows), "gamma.py has NO disposition"))

    # (b) a row whose tool is gone.
    r = dict(base_rows)
    r["deleted_tool.py"] = dict(ARCH_ROW)
    cases.append(
        (
            "(b) a row outliving its tool",
            dict(base_tools),
            r,
            "tools/deleted_tool.py, which does not exist",  # CITATION-OK
        )
    )

    # (c) a keep row nothing runs.
    t = dict(base_tools)
    t["orphan.py"] = "# orphan\n"
    r = dict(base_rows)
    r["orphan.py"] = dict(KEEP_ROW)
    cases.append(
        (
            "(c) a keep row with no invoker",
            t,
            r,
            "tools/orphan.py is KEEP but nothing runs it",  # CITATION-OK
        )
    )

    # (c') ...and a skill citation does NOT rescue it.
    cases.append(
        (
            "(c') a skill citation is not an invoker",
            t,
            r,
            "tools/orphan.py is KEEP but nothing runs it",  # CITATION-OK
            {
                ".claude/skills/demo/SKILL.md": "run `python tools/orphan.py --check`\n"  # CITATION-OK
            },
        )
    )

    # (d) hand_run pasted onto a gated tool.
    r = dict(base_rows)
    r["alpha.py"] = dict(KEEP_ROW, hand_run=True)
    cases.append(
        (
            "(d) hand_run on a gated tool",
            dict(base_tools),
            r,
            "marked hand_run, but tools/lint_repo.py invokes it",
        )
    )

    ok = True
    for case in cases:
        label, tools, rows, needle = case[:4]
        extra = case[4] if len(case) > 4 else None
        tmp = tempfile.mkdtemp(prefix="tooldisp_")
        try:
            _plant(tmp, tools, rows, LINT_OK, GATE_OK, extra)
            bad = check(tmp, verbose=False)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        hit = any(needle in b for b in bad)
        print("  %-42s %s" % (label, "CAUGHT" if hit else "MISSED"))
        if not hit:
            print("     wanted %r, got: %s" % (needle, bad or "(no complaint at all)"))
            ok = False

    # ---- the LEDGER arm (fork F5M S4b): the withhold rule equals the archive class -------------
    # Planted in BOTH directions plus the ordering trap, because each one fails differently and
    # only the middle one is loud (see check_ledger's docstring).
    def _led(globs, order="before"):
        arch = {"id": ARCHIVE_RULE, "disposition": "withhold", "globs": list(globs)}
        tree = {"id": TOOLS_TREE_RULE, "disposition": "publish", "globs": ["tools/**"]}
        return {"rules": [arch, tree] if order == "before" else [tree, arch]}

    led_rows = {
        "alpha.py": dict(KEEP_ROW),
        "beta.py": dict(KEEP_ROW),
        "lint_repo.py": dict(KEEP_ROW),
        "run_gate.py": dict(KEEP_ROW, hand_run=True),
        "dusty.py": dict(ARCH_ROW),
    }
    led_tools = dict(base_tools)
    led_tools["dusty.py"] = "# dusty\n"
    led_cases = [
        (
            "(f) an archive tool the rule forgot",
            _led([]),
            led_rows,
            "tools/dusty.py is class ARCHIVE but",  # CITATION-OK -- planted fixture name
        ),
        (
            "(g) the rule withholding a KEEP tool",
            _led(["tools/dusty.py", "tools/alpha.py"]),  # CITATION-OK -- planted fixture names
            led_rows,
            "withholds tools/alpha.py, whose disposition class is keep",  # CITATION-OK
        ),
        (
            "(h) the rule placed behind tools-tree",
            _led(["tools/dusty.py"], order="after"),  # CITATION-OK -- planted fixture name
            led_rows,
            "sits AFTER `tools-tree`",
        ),
    ]
    for label, ledger, rows_, needle in led_cases:
        tmp = tempfile.mkdtemp(prefix="tooldisp_")
        try:
            _plant(tmp, led_tools, dict(rows_), LINT_OK, GATE_OK, None, ledger)
            bad = check_ledger(tmp, verbose=False)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        hit = any(needle in b for b in bad)
        print("  %-42s %s" % (label, "CAUGHT" if hit else "MISSED"))
        if not hit:
            print("     wanted %r, got: %s" % (needle, bad or "(no complaint at all)"))
            ok = False

    tmp = tempfile.mkdtemp(prefix="tooldisp_")
    try:
        good = _led(["tools/dusty.py"])  # CITATION-OK -- planted fixture name
        _plant(tmp, led_tools, dict(led_rows), LINT_OK, GATE_OK, None, good)
        led_clean = check_ledger(tmp, verbose=False)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("  %-42s %s" % ("the clean planted ledger", "green" if not led_clean else "RED"))
    for b in led_clean:
        print("     " + b)
    if led_clean:
        ok = False

    # WALKER LIVENESS. A scan that visited nothing is refused rather than read as clean -- this is
    # the one case that cannot be planted, because what it plants is an ABSENCE (G106).
    tmp = tempfile.mkdtemp(prefix="tooldisp_")
    try:
        _plant(tmp, {}, dict(base_rows), None, None)
        empty = check(tmp, verbose=False)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    hit = any("found NO top-level tools" in b for b in empty)
    print("  %-42s %s" % ("(e) an empty tree is refused", "CAUGHT" if hit else "MISSED"))
    if not hit:
        ok = False
        print("     got: %s" % (empty or "(no complaint at all)"))

    # ...and the CLEAN planted tree must be green, or the cases above prove only that the checker
    # complains about everything.
    tmp = tempfile.mkdtemp(prefix="tooldisp_")
    try:
        _plant(tmp, dict(base_tools), dict(base_rows), LINT_OK, GATE_OK)
        clean = check(tmp, verbose=False)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("  %-42s %s" % ("the clean planted tree", "green" if not clean else "RED"))
    for b in clean:
        print("     " + b)

    live = check(REPO, verbose=False) + check_ledger(REPO, verbose=False)
    print("  %-42s %s" % ("the real tree", "green" if not live else "RED"))
    for b in live:
        print("     " + b)
    return 0 if ok and not clean and not live else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="the gate (this is also the default)")
    ap.add_argument("--wired", action="store_true", help="archive rows HEAD still wires")
    ap.add_argument("--evidence", action="store_true", help="per-tool evidence, as measured now")
    ap.add_argument(
        "--ledger",
        action="store_true",
        help="the publish ledger's archive-class withhold rule equals the archive class",
    )
    ap.add_argument("--selftest", action="store_true", help="the gate's own negative cases")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.ledger:
        bad = check_ledger(REPO)
        if bad:
            print("\nTHE ARCHIVE-CLASS CUT AND THE DISPOSITIONS DISAGREE:")
            for b in bad:
                print("  " + b)
            return 1
        return 0
    if args.evidence:
        evidence(REPO)
        return 0
    if args.wired:
        rows = wired(REPO)
        print(
            "%d archive tools are still wired at HEAD (the fork cut removes these too):" % len(rows)
        )
        for t, inv in rows:
            print("  %-34s %s" % (t, ", ".join(inv)))
        return 0
    bad = check(REPO)
    if bad:
        print("\nTOOL DISPOSITIONS ARE NOT TRUE:")
        for b in bad:
            print("  " + b)
        return 1
    print("every top-level tool is dispositioned, and every keep row names what runs it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
