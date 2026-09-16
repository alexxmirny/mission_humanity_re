#!/usr/bin/env python3
"""lint_citations.py -- fork F5E: no PUBLISHED file may point a reader at a path the public tree
does not carry, and no file may point at a path that does not exist at all.

WHY THIS IS A SEPARATE TOOL FROM check_publishable --closure. They ask different questions and the
difference is not cosmetic. `--closure` asks a BUILD question -- "can a published source COMPILE or
RUN without a non-published path" -- and its scanner is deliberately narrow for that: C/C++
`#include "..."` lines only, and for Python only `ast` string constants (so comments are gone by
construction) plus reconstructed `os.path.join` chains, with a hard `tok.count("/") >= 1` floor whose
stated reason is that a bare basename in a Python string is prose. Every one of those choices is
right for a build edge and wrong for a CITATION: citations live in COMMENTS and PROSE, which that
scanner cannot see by construction, and in `.md` files, which it never opens. Widening it would
either break its precision or bolt a second mode onto a tool whose HARD / CONDITIONAL /
declared-exception semantics are tuned to build edges.

So this file imports `check_publishable` as a LIBRARY -- one ledger, one glob resolver, one suffix
index, two questions. The overlap on Python string constants is accepted and harmless: `--closure`
is the gate that fails the BUILD, this is the gate that fails the PROSE.

Precedent for the shape of the answer is the address-keyed doc-ref resolver ("a reference is checkable when
it carries an address; a bare name is prose"). Same rule, different axis: A PATH IS A LITERAL, SO IT
IS CHECKABLE; A BARE BASENAME IS PROSE -- except when it resolves uniquely and non-generically to a
tracked private file, which is a finding, not prose (see BARE BASENAMES below).

---- THE THREE DIRECTIONS, AND WHY ONLY TWO OF THEM ARE HARD -------------------------------------

  DANGLING   a citation of a path that is not tracked, not on disk and not gitignored.
             HARD IN BOTH TREES from day one. A dangling path is a bug in the private tree too --
             the target moved or was deleted and the sentence now sends its reader nowhere.

  PRIVATE from PROSE (`.md`)            "Direction A" -- HARD from day one WHEN THE TARGET IS
             `withhold`. A published document pointing at a document the reader can never open is
             precisely the defect the public cut is about. A citation of a `user_pending` target is
             CONDITIONAL instead -- grouped, counted, never failed -- the same HARD/CONDITIONAL
             split check_publishable --closure applies to build edges, and for the same reason: it
             is the measured blast radius of an open question, and failing it would force the drain
             to delete references the user may be about to rule publishable.

  PRIVATE from CODE/DATA (everything else)   "Direction B" -- a PER-FILE, TIGHTENING-ONLY RATCHET
             against tools/data/citation_baseline.json, exactly like lint_source_narration's. The
             volume is ~1200 lines over ~450 files; a hard gate on day one would mean the whole
             drain has to land in one commit or lint_repo is red throughout. NEW FILES START AT 0,
             so a NEW citation into a private layer is an immediate red -- which is the property
             that matters: the tree stops getting worse the moment the row lands.

  UNTRACKED-BY-DESIGN   a citation of a path that is not tracked but IS on disk or gitignored
             (build outputs, tools/machine.local.json, the Ghidra DB, the loop's telemetry).  # CITATION-OK
             Reported under --strict, never failed.

---- THE TEN RESOLUTION RULES, IN ORDER ----------------------------------------------------------

  1. Skip the whole LINE if it carries the escape `CITATION-OK` (the NARRATION-OK precedent: the
     escape suppresses EVERY token on its line, and only on that line).
  2. Strip URLs, then tokenise. Normalise each token: `\\` -> `/`, strip a leading `./` and a
     trailing `/`, and strip trailing `.,;:)]}'"` -- but BALANCE-AWARE: a `}` is not stripped while
     an unmatched `{` is still open, or `save_block.{h,cpp}` reads as dangling.
  2b. IN A .md SOURCE ONLY: a markdown link target is a path RELATIVE TO THE CONTAINING FILE'S
     DIRECTORY, resolved as such (see md_links). Rule 8 refuses an ambiguous bare basename on
     purpose, and a doc-to-doc link is a bare basename by convention -- so without this a
     published doc can link into a withheld one and no token rule sees it (measured at the Q4
     flip: 55 lines). Yields TRACKED targets only, so it reports PRIVATE, never DANGLING.
  3. Brace-expand: `save_block.{h,cpp}` -> two candidates. A brace group with NO COMMA is a template
     placeholder (`{DOMAIN}`, `{domain}`), not an alternation -> skip the token entirely.
  4. Exact hit in `git ls-files` -> resolved.
  5. Token is a tracked DIRECTORY prefix -> resolved to its whole member set.
  6. Token contains `* ? [` -> glob (check_publishable.glob_re) over the tracked set; >=1 member
     -> resolved.
  7. The token's last segment has NO `.` -> try `token + ".md"` (the house form
     `docs/mp-replay-harness`); otherwise SKIP, do not flag. This is the prose-false-positive killer  # CITATION-OK
     -- it is what keeps `src/dst`, `tools/call` (a JSON-RPC method name) and `src/-only` out. Its
     stated cost: a citation of a real, untracked, extension-less directory goes unchecked.
  8. Suffix-index hit (check_publishable.suffix_index, whose unique-basename and
     non-generic-basename rules are reused verbatim) -> resolved.
  9. Nothing resolves -> DANGLING; then downgrade to UNTRACKED-BY-DESIGN if the path exists on disk
     or `git check-ignore` matches it.

  VERDICT. PRIVATE = resolved, and NO member of the resolution set is in the publish set -- the same
  "satisfiable if any hit publishes" rule check_publishable._resolve applies to includes.

---- BARE BASENAMES: measured, asymmetric on purpose ----------------------------------------------

Enabling bare basenames (`CLAUDE.md`, `mh_export.gen.h`, `dll_call_protos.json`) as tokens adds  # CITATION-OK
~383 genuine PRIVATE findings and ~1393 junk DANGLING ones (`mh_net.ini` x130, `windows.h` x74, the
selftest placeholders `a.cpp`/`b.cpp`/`c.h`) -- unusable at any allowlist size. So the rule is
ASYMMETRIC: a bare basename that resolves uniquely and non-generically to a tracked private path IS
a finding; one that resolves to nothing is prose and is dropped silently. `--no-bare` turns the
whole class off.

---- THE ALLOWLIST, AND WHY IT CANNOT BECOME A LANDFILL -------------------------------------------

tools/data/citation_allowlist.json carries ordered {id, sources[], targets[], why} rules. `why` is
mandatory, and -- carrying the publish ledger's arm-3 discipline over verbatim -- A RULE THAT
MATCHES ZERO LIVE VIOLATIONS FAILS THE GATE. That is what stops the allowlist from becoming the
place violations go to be forgotten. It is for RULED COUPLINGS, not for unfixed lines.

usage:
  python tools/lint_citations.py                    # the gate: dangling + prose hard, code ratcheted
  python tools/lint_citations.py --dangling         # just the dangling arm, listed
  python tools/lint_citations.py --prose            # just Direction A (hard + the conditional roll-up)
  python tools/lint_citations.py --code             # just Direction B, against the baseline
  python tools/lint_citations.py --report           # the whole picture: counts, top sources/targets
  python tools/lint_citations.py --update-baseline  # tighten the Direction B ratchet (never raises)
  python tools/lint_citations.py --selftest         # the planted negatives still fire
"""

from __future__ import annotations

import argparse
import json
import os
import posixpath
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

import check_publishable as cp  # noqa: E402

BASELINE = os.path.join(REPO, "tools", "data", "citation_baseline.json")
ALLOWLIST = os.path.join(REPO, "tools", "data", "citation_allowlist.json")

ESCAPE = "CITATION-OK"

# Anchoring on a KNOWN TOP-LEVEL DIRECTORY rather than "any token with a slash" is what keeps
# `src/dst`, `tools/call` and `src/-only` from even being considered.
TOP = r"docs|tools|src|tasks|notes|session_reports|tracker|mod_src|report|db|\.claude|\.github"
SEG = r"[A-Za-z0-9_.+*?{},\[\]-]"
PATH_RE = re.compile(r"(?<![A-Za-z0-9_./\\-])((?:%s)/(?:%s+/)*%s*)" % (TOP, SEG, SEG))

# README.md and LICENSE are deliberately NOT here -- README.md is already in
# check_publishable.GENERIC_BASENAMES for the same reason (5 tracked READMEs; a bare one names none
# of them), and LICENSE is a word.
ROOT_DOCS = [
    "NOTES.md",  # CITATION-OK
    "TASKS.md",  # CITATION-OK
    "ROADMAP.md",  # CITATION-OK
    "CLAUDE.md",  # CITATION-OK
    "INSTALL.md",
    "THIRD_PARTY.md",
    "BNK_FORMAT.md",  # CITATION-OK
]
ROOT_RE = re.compile(r"(?<![A-Za-z0-9_./\\-])(%s)\b" % "|".join(re.escape(x) for x in ROOT_DOCS))
BARE_RE = re.compile(
    r"(?<![A-Za-z0-9_./\\-])([A-Za-z0-9_][A-Za-z0-9_.+-]*"
    r"\.(?:py|cpp|hpp|h|cc|md|json|ps1|js|yaml|yml|bat|ini))\b"
)
URL_RE = re.compile(r"https?://\S+")

# `.json` IS DELIBERATELY ABSENT. tools/data/*.json is GENERATED-AND-GATED evidence -- the
# `why`/`evidence` fields of sim_migration.json, ghidra_findings.json, tool_dispositions.json and
# this ledger's own rows name the private layer BECAUSE THAT IS WHAT THEY ARE RECORDING. Scanning
# them adds ~1000 lines nobody can "re-author" (the fix would be to delete the evidence) and buries
# the class this lint is for: a sentence a human wrote, pointing a human reader somewhere.
TEXT_EXT = (
    ".md",
    ".cpp",
    ".h",
    ".c",
    ".cc",
    ".hpp",
    ".py",
    ".ps1",
    ".txt",
    ".js",
    ".bat",
    ".yaml",
    ".yml",
    ".cfg",
    ".ini",
)
PROSE_EXT = (".md",)
CODE_EXT = (".py", ".cpp", ".h", ".c", ".cc", ".hpp", ".ps1", ".js", ".bat")

PRIVATE, DANGLING, UNTRACKED = "private", "dangling", "untracked"


# --------------------------------------------------------------------------- tokenising


def expand_braces(tok):
    """`x.{h,cpp}` -> ['x.h', 'x.cpp']. A group with no comma is a TEMPLATE placeholder, not an
    alternation, so the whole token is dropped (returns [])."""
    m = re.search(r"\{([^{}]*)\}", tok)
    if not m:
        return [tok]
    if "," not in m.group(1):
        return []
    out = []
    for alt in m.group(1).split(","):
        out.extend(expand_braces(tok[: m.start()] + alt + tok[m.end() :]))
    return out


_CLOSERS = {")": "(", "]": "[", "}": "{"}


def norm(tok):
    """Normalise one raw token. The trailing-punctuation strip is BALANCE-AWARE: this was the
    prototype's one real bug -- `save_block.{h,cpp}` lost its closing brace and read as dangling."""
    tok = tok.replace("\\", "/").strip()
    while tok:
        c = tok[-1]
        if c in ".,;:'\"`":
            tok = tok[:-1]
            continue
        if c in _CLOSERS:
            opener = _CLOSERS[c]
            body = tok[:-1]
            if body.count(opener) > body.count(c):
                break  # the closer balances an opener inside the token: it belongs to the path
            tok = body
            continue
        break
    if tok.startswith("./"):
        tok = tok[2:]
    if tok.endswith("/"):
        tok = tok[:-1]
    return tok


def tokens_in(line, include_bare):
    """[(token, kind)] for one line. URLs are stripped first; a bare basename lying inside an
    already-matched path token is not re-reported."""
    clean = URL_RE.sub(" ", line)
    spans = [m.span(1) for m in PATH_RE.finditer(clean)]
    out = [(m.group(1), "path") for m in PATH_RE.finditer(clean)]
    out += [(m.group(1), "root") for m in ROOT_RE.finditer(clean)]
    if include_bare:
        for m in BARE_RE.finditer(clean):
            if any(a <= m.start(1) < b for a, b in spans):
                continue
            out.append((m.group(1), "bare"))
    return out


# --------------------------------------------------------------------------- resolution


class Index:
    """The tracked file set, its directory prefixes, and check_publishable's own suffix index."""

    def __init__(self, paths):
        self.tracked = set(paths)
        self.dirs = set()
        for p in paths:
            segs = p.split("/")
            for i in range(1, len(segs)):
                self.dirs.add("/".join(segs[:i]))
        self.suffix = cp.suffix_index(paths)

    def _one(self, t):
        """-> (kind, targets); kind in tracked | noext | none."""
        if t in self.tracked:  # rule 4
            return ("tracked", [t])
        if t in self.dirs:  # rule 5
            return ("tracked", [p for p in self.tracked if p.startswith(t + "/")])
        if any(c in t for c in "*?["):  # rule 6
            try:
                rx = cp.glob_re(t)
            except re.error:
                return ("none", [])
            hits = [p for p in self.tracked if rx.match(p)]
            return ("tracked", hits) if hits else ("none", [])
        if "." not in t.rsplit("/", 1)[-1]:  # rule 7
            if t + ".md" in self.tracked:
                return ("tracked", [t + ".md"])
            return ("noext", [])
        hits = self.suffix.get(t)  # rule 8
        if hits:
            return ("tracked", sorted(hits))
        return ("none", [])

    def resolve(self, tok):
        """-> [(candidate, kind, targets)], kind in ok | noext | none.

        Brace expansion (rule 3) yields ONE VERDICT PER CANDIDATE, not one unioned verdict:
        `save_block.{h,cpp}` is two citations written short, so a published `.h` does not excuse a
        withheld `.cpp`. A GLOB is the opposite -- `docs/*.md` is ONE reference that any matching
        member satisfies -- which is why rule 6 unions and rule 3 does not."""
        cands = expand_braces(tok)
        if not cands:
            return [(tok, "noext", [])]  # a template placeholder: skipped, exactly like rule 7
        out = []
        for c in cands:
            k, t = self._one(c)
            out.append((c, "ok" if k == "tracked" else k, t))
        return out


def gitignored_many(tokens, root=REPO):
    """One `git check-ignore --stdin` call, not one per token."""
    tokens = [t for t in tokens if t]
    if not tokens:
        return set()
    try:
        # BYTES, not text=True. On Windows the text-mode pipe rewrites "\n" to "\r\n", git then
        # sees a CR inside the pathname and answers with a C-QUOTED path ("...stop.request/r"),
        # which matches nothing on the way back and silently turns every ignored path into a
        # DANGLING report. Measured on session_reports/loop/stop.request.  # CITATION-OK
        pr = subprocess.run(
            ["git", "-C", root, "check-ignore", "--stdin"],
            input=("\n".join(tokens) + "\n").encode("utf-8"),
            capture_output=True,
        )
    except OSError:
        return set()
    out = pr.stdout.decode("utf-8", "replace")
    return {ln.strip().replace("\\", "/") for ln in out.splitlines() if ln.strip()}


# --------------------------------------------------------------------------- the scan


MD_LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+?)(?:#[^)\s]*)?\)")


def md_links(src, line, tracked):
    """RULE 2b -- a markdown link target inside a .md file is a path RELATIVE TO THAT FILE'S
    DIRECTORY, and inside a link there is nothing to be conservative about: the renderer resolves it
    against the containing directory and so do we.

    This exists because rule 8 deliberately REFUSES an ambiguous bare basename -- `gfx-sprites.md`
    names both docs/ and tasks/, so check_publishable.suffix_index records neither -- and a markdown  # CITATION-OK
    link is exactly where that conservatism is wrong. Measured when Q4 flipped the research docs to
    withhold: 55 lines in PUBLISHED documents linked into WITHHELD ones and no token rule could see
    one of them, because a doc-to-doc link is a bare basename by convention.

    DELIBERATE LIMIT: this yields only TRACKED targets, so it reports PRIVATE and never DANGLING. A
    relative link that resolves to nothing is a broken-link check -- a different gate, which would
    fire on every anchor-only and generated-path link in the tree."""
    d = posixpath.dirname(src)
    for m in MD_LINK.finditer(line):
        t = m.group(1)
        if not t or t[0] in "<#" or "://" in t or t.startswith("mailto:"):
            continue
        tgt = posixpath.normpath(posixpath.join(d, t) if d else t).replace("\\", "/")
        if tgt in tracked:
            yield tgt


def scan(published, idx, root=REPO, files=None, include_bare=True, check_disk=True):
    """-> (violations, visited). A violation is a dict: file, line, token, status, target, kind."""
    files = sorted(published) if files is None else list(files)
    raw = []
    visited = 0
    for p in files:
        if not p.lower().endswith(TEXT_EXT):
            continue
        full = os.path.join(root, p.replace("/", os.sep))
        if not os.path.isfile(full):
            continue
        try:
            with open(full, encoding="utf-8", errors="replace") as fh:
                lines = fh.readlines()
        except OSError:
            continue
        visited += 1
        for n, line in enumerate(lines, 1):
            if ESCAPE in line:  # rule 1
                continue
            if p.lower().endswith(PROSE_EXT):
                for tgt in md_links(p, line, idx.tracked):  # rule 2b
                    if tgt in published:
                        continue
                    raw.append(
                        {
                            "file": p,
                            "line": n,
                            "token": tgt,
                            "status": PRIVATE,
                            "target": tgt,
                            "kind": "mdlink",
                        }
                    )
            for tok, kind in tokens_in(line, include_bare):  # rule 2
                t = norm(tok)
                if not t:
                    continue
                for cand, k, targets in idx.resolve(t):
                    if k == "noext":
                        continue
                    if k == "none":
                        if kind == "bare":
                            continue  # an unresolvable bare basename is PROSE, never dangling
                        raw.append(
                            {
                                "file": p,
                                "line": n,
                                "token": cand,
                                "status": DANGLING,
                                "target": "",
                                "kind": kind,
                            }
                        )
                        continue
                    if any(x in published for x in targets):
                        continue
                    raw.append(
                        {
                            "file": p,
                            "line": n,
                            "token": cand,
                            "status": PRIVATE,
                            "target": sorted(targets)[0] if targets else "",
                            "kind": kind,
                        }
                    )

    # rule 9's downgrade, batched.
    if check_disk:
        cand = sorted({r["token"] for r in raw if r["status"] == DANGLING})
        ignored = gitignored_many(cand, root)
        for r in raw:
            if r["status"] != DANGLING:
                continue
            t = r["token"]
            if t in ignored or os.path.exists(os.path.join(root, t.replace("/", os.sep))):
                r["status"] = UNTRACKED

    # One record per (file, line, token): a markdown link spells its target twice.
    seen, out = set(), []
    for r in raw:
        key = (r["file"], r["line"], r["token"], r["status"])
        if key in seen:
            continue
        seen.add(key)
        out.append(r)
    return out, visited


# --------------------------------------------------------------------------- allowlist


def load_allowlist(path=ALLOWLIST):
    if not os.path.isfile(path):
        return []
    with open(path, encoding="utf-8") as fh:
        doc = json.load(fh)
    rules = doc.get("rules", [])
    for r in rules:
        if not r.get("id"):
            raise SystemExit("citation allowlist: a rule has no id")
        if not r.get("why"):
            raise SystemExit(
                "citation allowlist: rule %s carries no `why` -- an exemption without a reason "
                "is not a declaration" % r["id"]
            )
        r["_src"] = [cp.glob_re(g) for g in r.get("sources", [])]
        r["_tgt"] = [cp.glob_re(g) for g in r.get("targets", [])]
        r["_hit"] = 0
    return rules


def apply_allowlist(violations, rules):
    """-> kept violations. Mutates each rule's _hit count (arm: a rule with 0 hits FAILS)."""
    for r in rules:
        r["_hit"] = 0
    kept = []
    for v in violations:
        for r in rules:
            if any(rx.match(v["file"]) for rx in r["_src"]) and (
                not r["_tgt"] or any(rx.match(v["target"] or v["token"]) for rx in r["_tgt"])
            ):
                r["_hit"] += 1
                break
        else:
            kept.append(v)
    return kept


# --------------------------------------------------------------------------- baseline


def load_baseline(path=BASELINE):
    try:
        with open(path, encoding="utf-8") as fh:
            return {k: v for k, v in json.load(fh).items() if not k.startswith("_")}
    except (OSError, ValueError):
        return {}


def code_counts(violations):
    """{file: distinct private LINES} over non-.md sources -- Direction B's ratchet unit."""
    per = {}
    for v in violations:
        if v["status"] != PRIVATE or v["file"].lower().endswith(PROSE_EXT):
            continue
        per.setdefault(v["file"], set()).add(v["line"])
    return {k: len(s) for k, s in sorted(per.items())}


def update_baseline(counts, path=BASELINE):
    old = load_baseline(path)
    raised = [p for p, n in counts.items() if n > old.get(p, 0)] if old else []
    if raised:
        sys.exit(
            "refusing to RAISE the citation baseline for: %s\nThe ratchet only tightens -- "
            "re-author the citation (inline the finding), or mark a ruled coupling in "
            "tools/data/citation_allowlist.json." % ", ".join(sorted(raised)[:20])
        )
    doc = {
        "_comment": "Ratchet baseline for tools/lint_citations.py DIRECTION B (a published "
        "CODE/DATA file citing a path the public tree does not carry): per-file counts of distinct "
        "violating lines. The lint fails a file that EXCEEDS its count; a file with no entry is "
        "allowed 0, so a NEW citation into a private layer is an immediate red. --update-baseline "
        "only lowers entries. Drain a file and regenerate in the same commit. Direction A "
        "(published PROSE) and DANGLING are hard and have no baseline."
    }
    doc.update(counts)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, indent=2)
        fh.write("\n")
    dropped = sorted(set(old) - set(counts))
    print(
        "citation baseline updated: %d file(s), %d line(s)%s"
        % (
            len(counts),
            sum(counts.values()),
            "; dropped %d now-clean file(s)" % len(dropped) if dropped else "",
        )
    )


# --------------------------------------------------------------------------- the arms


def collect(root=REPO, include_bare=True):
    """-> (violations after the allowlist, allowlist rules, visited, published set).

    Each violation also carries `disp`, the LEDGER DISPOSITION of its target, because Direction A
    splits on it the way check_publishable --closure splits its edges (see _prose)."""
    led = cp.load_ledger()
    paths = cp.tracked_paths(root)
    published = set(cp.publish_list(paths, led))
    idx = Index(paths)
    vios, visited = scan(published, idx, root=root, include_bare=include_bare)
    by_path, _, _ = cp.classify(paths, led)
    for v in vios:
        r = by_path.get(v["target"])
        v["disp"] = r["disposition"] if r else ""
        v["question"] = (r or {}).get("question") or (r or {}).get("blocked_on") or "?"
    rules = load_allowlist()
    return apply_allowlist(vios, rules), rules, visited, published


def _dangling(vios, say):
    bad = [v for v in vios if v["status"] == DANGLING]
    for v in sorted(bad, key=lambda v: (v["file"], v["line"])):
        say(
            "[citation/dangling] %s:%d  %s -- not tracked, not on disk, not ignored"
            % (v["file"], v["line"], v["token"])
        )
    return bad


def _prose(vios, say, show_conditional=True):
    """Direction A, split HARD vs CONDITIONAL exactly as check_publishable.closure splits its edges.

    A published document pointing at a WITHHELD path is a contradiction TODAY: the target is not
    coming back, so the sentence can never be followed. That FAILS.

    A published document pointing at a `user_pending` path is a contradiction ONLY IF the answer
    goes the other way -- it is the measured blast radius of an open question, not a defect. Failing
    it would force the drain to strip references the user may be about to rule publishable, which is
    exactly the "lose the finding" mistake this item exists to avoid. THIRD_PARTY.md citing
    BNK_FORMAT.md (Q2) is the clearest case: that citation IS the attribution the row asks for.  # CITATION-OK
    So they are grouped, counted and reported -- and F5M is where they must be zero."""
    prose = [v for v in vios if v["status"] == PRIVATE and v["file"].lower().endswith(PROSE_EXT)]
    hard = [v for v in prose if v["disp"] == "withhold"]
    cond = [v for v in prose if v["disp"] != "withhold"]
    for v in sorted(hard, key=lambda v: (v["file"], v["line"])):
        say(
            "[citation/prose] %s:%d  %s -- a PUBLISHED document pointing at a WITHHELD path (%s)"
            % (v["file"], v["line"], v["token"], v["target"] or "?")
        )
    if cond and show_conditional:
        groups = {}
        for v in cond:
            groups.setdefault(v["question"], {}).setdefault(v["target"], set()).add(v["file"])
        say("")
        say("CONDITIONAL prose citations -- contradictions only if the answer is 'withhold':")
        for key in sorted(groups):
            tot = sum(len(s) for s in groups[key].values())
            say(
                "  %-5s %3d citation(s) of %d target(s), from %d document(s)"
                % (
                    key,
                    tot,
                    len(groups[key]),
                    len({f for m in groups[key].values() for f in m}),
                )
            )
    return hard


def _code(vios, say):
    counts = code_counts(vios)
    base = load_baseline()
    probs = []
    for path, n in counts.items():
        allowed = base.get(path, 0)
        if n > allowed:
            lines = sorted(
                {v["line"] for v in vios if v["file"] == path and v["status"] == PRIVATE}
            )
            shown = ", ".join(str(x) for x in lines[:8]) + (" ..." if len(lines) > 8 else "")
            probs.append(
                "[citation/code] %s: %d line(s) citing a non-published path, baseline %d "
                "(lines %s) -- inline the finding, don't just drop the pointer"
                % (path, n, allowed, shown)
            )
    for p in probs:
        say(p)
    return probs, counts


def _allowlist_arm(rules, say):
    stale = [r for r in rules if not r["_hit"]]
    for r in stale:
        say(
            "[citation/allowlist] rule %s matches ZERO live violations -- the coupling it excuses "
            "is gone; DELETE the row (blocked_on %s)" % (r["id"], r.get("blocked_on", "?"))
        )
    return stale


def gate(root=REPO, say=print):
    vios, rules, visited, _pub = collect(root)
    ok = True
    if visited <= 100:
        say(
            "FAIL walker liveness: the citation scan visited only %d published text file(s)."
            % visited
        )
        say("     A scan that finds nothing and a scan that is broken look identical.")
        return False
    bad_d = _dangling(vios, say)
    bad_p = _prose(vios, say)
    probs_c, counts = _code(vios, say)
    stale = _allowlist_arm(rules, say)
    if bad_d:
        ok = False
        say("FAIL dangling: %d citation(s) name a path that does not exist." % len(bad_d))
    if bad_p:
        ok = False
        say("FAIL prose: %d published DOCUMENT line(s) cite a WITHHELD path." % len(bad_p))
    if probs_c:
        ok = False
        say("FAIL code ratchet: %d file(s) exceed their citation baseline." % len(probs_c))
    if stale:
        ok = False
        say("FAIL stale allowlist: %d rule(s) match nothing." % len(stale))
    say(
        "lint_citations: scanned %d published file(s); dangling %d, prose %d, code %d line(s) over "
        "%d file(s) (baseline %d) -- %s"
        % (
            visited,
            len(bad_d),
            len(bad_p),
            sum(counts.values()),
            len(counts),
            sum(load_baseline().values()),
            "PASS" if ok else "FAIL",
        )
    )
    return ok


def report(root=REPO, say=print, strict=False):
    vios, rules, visited, published = collect(root)
    kinds = {"code": 0, "prose": 0, "data": 0}
    lines = {"code": set(), "prose": set(), "data": set()}
    for v in vios:
        if v["status"] != PRIVATE:
            continue
        ext = os.path.splitext(v["file"])[1].lower()
        k = "prose" if ext in PROSE_EXT else ("code" if ext in CODE_EXT else "data")
        kinds[k] += 1
        lines[k].add((v["file"], v["line"]))
    dang = [v for v in vios if v["status"] == DANGLING]
    untr = [v for v in vios if v["status"] == UNTRACKED]
    say("scanned %d published text file(s) out of %d published path(s)" % (visited, len(published)))
    say("")
    say("| direction                                   | lines | tokens |")
    say("| ------------------------------------------- | ----: | -----: |")
    say(
        "| PRIVATE, from code (.py/.cpp/.h/.ps1/.js)   | %5d | %6d |"
        % (len(lines["code"]), kinds["code"])
    )
    say(
        "| PRIVATE, from prose (.md)                   | %5d | %6d |"
        % (len(lines["prose"]), kinds["prose"])
    )
    say(
        "| PRIVATE, from data (.ini/.txt/.yml/.cfg)    | %5d | %6d |"
        % (len(lines["data"]), kinds["data"])
    )
    say(
        "| DANGLING                                    | %5d | %6d |"
        % (len({(v["file"], v["line"]) for v in dang}), len(dang))
    )
    say(
        "| UNTRACKED (info only)                       | %5d | %6d |"
        % (len({(v["file"], v["line"]) for v in untr}), len(untr))
    )
    say("")
    src = {}
    tgt = {}
    for v in vios:
        if v["status"] != PRIVATE:
            continue
        src[v["file"]] = src.get(v["file"], 0) + 1
        tgt[v["target"]] = tgt.get(v["target"], 0) + 1
    say("top source files:")
    for f, c in sorted(src.items(), key=lambda kv: (-kv[1], kv[0]))[:12]:
        say("  %4d  %s" % (c, f))
    say("top targets:")
    for f, c in sorted(tgt.items(), key=lambda kv: (-kv[1], kv[0]))[:12]:
        say("  %4d  %s" % (c, f))
    if rules:
        say("")
        say("allowlist:")
        for r in rules:
            say("  %-40s %4d live violation(s)" % (r["id"], r["_hit"]))
    if strict and untr:
        say("")
        say("UNTRACKED-BY-DESIGN (never failed):")
        for v in sorted(untr, key=lambda v: (v["file"], v["line"]))[:60]:
            say("  %s:%d  %s" % (v["file"], v["line"], v["token"]))
    return 0


# --------------------------------------------------------------------------- selftest


def _synth_ledger(rules):
    led = {"rules": rules, "closure_exceptions": []}
    for r in led["rules"]:
        r["_res"] = [cp.glob_re(g) for g in r["globs"]]
    return led


def selftest():
    """Every planted case must FAIL when planted and PASS when removed. A checker nobody has seen
    fail is a checker nobody tested."""
    import shutil
    import tempfile

    ok = True
    results = []

    def expect(name, cond):
        nonlocal ok
        results.append((name, bool(cond)))
        if not cond:
            ok = False

    tmp = tempfile.mkdtemp(prefix="lint_citations_selftest_")
    try:

        def write(rel, content):
            full = os.path.join(tmp, rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(content)

        tracked = [
            "docs/zz-withheld-fixture.md",  # CITATION-OK
            "docs/architecture.md",
            "docs/zz-noext-fixture.md",  # CITATION-OK
            "docs/zz-amb.md",  # CITATION-OK
            "tasks/zz-amb.md",  # CITATION-OK
            "tasks/foo.md",  # CITATION-OK
            "tools/pub.py",  # CITATION-OK
            "docs/pub.md",  # CITATION-OK
            "src/x/save_block.h",  # CITATION-OK
            "src/x/save_block.cpp",  # CITATION-OK
        ]
        for t in tracked:
            write(t, "seed\n")
        write("build/out.txt", "ignored-but-present\n")
        rules = [
            {
                "id": "private",
                "disposition": "withhold",
                "globs": [
                    "docs/zz-withheld-fixture.md",  # CITATION-OK
                    "docs/zz-amb.md",  # CITATION-OK
                    "tasks/**",  # CITATION-OK
                    "src/x/save_block.cpp",  # CITATION-OK
                ],  # CITATION-OK
                "why": "x",
            },
            {"id": "pub", "disposition": "publish", "globs": ["**"], "why": "x"},
        ]
        led = _synth_ledger(rules)
        published = set(cp.publish_list(tracked, led))
        idx = Index(tracked)

        def run(files=None, bare=True):
            return scan(
                published,
                idx,
                root=tmp,
                files=files or ["docs/pub.md", "tools/pub.py"],  # CITATION-OK
                include_bare=bare,
            )[0]

        PUB_PY = "tools/pub.py"  # CITATION-OK

        def statuses(vios, token=None):
            return {v["status"] for v in vios if token is None or v["token"] == token}

        # 1. a published .md citing a withheld doc is PRIVATE
        write("docs/pub.md", "see docs/zz-withheld-fixture.md for the trap\n")  # CITATION-OK
        write("tools/pub.py", "X = 1\n")  # CITATION-OK
        v = run()
        expect(
            "(1) published .md -> withheld doc is PRIVATE",
            statuses(v, "docs/zz-withheld-fixture.md") == {PRIVATE},  # CITATION-OK
        )
        write("docs/pub.md", "see docs/architecture.md for the design\n")  # CITATION-OK
        expect("(1') ...and the same shape into a PUBLISHED doc is silent", not run())

        # 2. a published .py COMMENT citing a withheld path is PRIVATE (check_publishable cannot
        #    see this by construction -- it is the whole reason this tool exists)
        write("tools/pub.py", "# per tasks/foo.md the knob is off\nX = 1\n")  # CITATION-OK
        expect(
            "(2) a published .py COMMENT -> withheld path is PRIVATE",
            statuses(run(), "tasks/foo.md") == {PRIVATE},  # CITATION-OK
        )
        write("tools/pub.py", "X = 1\n")  # CITATION-OK
        expect("(2') ...and the comment removed is silent", not run())

        # 3. a path nothing backs is DANGLING
        write("tools/pub.py", "# see tools/deleted_tool.py\n")  # CITATION-OK
        expect("(3) an unbacked path is DANGLING", statuses(run()) == {DANGLING})

        # 4. a gitignored-or-present path is UNTRACKED, not DANGLING
        write("tools/pub.py", "# output lands in report/x.html\n")  # CITATION-OK
        write("report/x.html", "present but untracked\n")  # CITATION-OK
        expect(
            "(4) a present-but-untracked path downgrades to UNTRACKED",
            statuses(run()) == {UNTRACKED},
        )

        # 5. CITATION-OK suppresses the line, and ONLY that line
        write(
            "tools/pub.py",  # CITATION-OK
            "# see tools/deleted_tool.py  CITATION-OK (a fixture path)\n"
            "# see tools/other_missing.py\n",  # CITATION-OK
        )
        v = run()
        expect("(5) CITATION-OK suppresses its own line", not [x for x in v if x["line"] == 1])
        expect("(5') ...and only its own line", [x for x in v if x["line"] == 2])

        # 6. brace expansion: x.{h,cpp} where only the .h publishes is still PRIVATE
        CPP_HALF = "src/x/save_block.cpp"  # CITATION-OK
        write("tools/pub.py", "# both halves: src/x/save_block.{h,cpp}\n")  # CITATION-OK
        v = run()
        expect(
            "(6) brace expansion sees the withheld half",
            any(x["status"] == PRIVATE and x["target"] == CPP_HALF for x in v),
        )
        expect(
            "(6') ...and does not report the token as dangling",
            not [x for x in v if x["status"] == DANGLING],
        )

        # 7. a comma-less brace group is a TEMPLATE, skipped not flagged
        write("tools/pub.py", '# writes tracker/{domain}.yaml\n')  # CITATION-OK
        expect("(7) a template brace group is skipped, not flagged", not run())

        # 8. the no-extension rule
        write("tools/pub.py", "# copy src/dst and call tools/call\n")  # CITATION-OK
        expect("(8) extension-less tokens produce no verdict", not run())
        write("docs/pub.md", "see docs/zz-noext-fixture for the shape\n")  # CITATION-OK
        expect(
            "(8') ...but the house `docs/foo` form resolves to the .md",
            not [x for x in run() if x["file"] == "docs/pub.md"],  # CITATION-OK
        )
        write("docs/pub.md", "x\n")  # CITATION-OK

        # 8b. rule 2b -- a markdown link resolves against the containing file's directory
        write("docs/pub.md", "the shape is in [amb](zz-amb.md)\n")  # CITATION-OK
        expect(
            "(8b) a relative markdown link into a WITHHELD doc is reported",
            statuses(run(), "docs/zz-amb.md") == {PRIVATE},  # CITATION-OK
        )
        expect(
            "(8b') ...which the TOKEN rules cannot see (the basename is ambiguous)",
            not [x for x in run() if x["kind"] != "mdlink"],
        )
        write("docs/pub.md", "the shape is in [pub](../docs/pub.md)\n")  # CITATION-OK
        expect("(8b'') a link to a PUBLISHED doc is silent", not run())
        write("docs/pub.md", "[x](https://e.invalid/x.md) [a](#a)\n")  # CITATION-OK
        expect("(8b''') external and anchor-only links are skipped", not run())
        write("docs/pub.md", "x\n")  # CITATION-OK

        # 9. bare basenames are asymmetric
        write("tools/pub.py", "# the convention lives in zz-withheld-fixture.md\n")  # CITATION-OK
        expect(
            "(9) a unique private bare basename IS reported",
            statuses(run(), "zz-withheld-fixture.md") == {PRIVATE},  # CITATION-OK
        )
        write("tools/pub.py", "# include windows.h and mh_net.ini\n")  # CITATION-OK
        expect("(9') an unresolvable bare basename is silent (prose)", not run())
        expect("(9'') ...and --no-bare turns the whole class off", not run(bare=False))

        # 12. an allowlist rule that matches nothing FAILS
        allow = [
            {"id": "live", "sources": [PUB_PY], "targets": ["tasks/**"], "why": "w"},  # CITATION-OK
            {"id": "dead", "sources": ["tools/gone.py"], "targets": ["tasks/**"], "why": "w"},  # noqa: E501  CITATION-OK
        ]
        for r in allow:
            r["_src"] = [cp.glob_re(g) for g in r["sources"]]
            r["_tgt"] = [cp.glob_re(g) for g in r["targets"]]
            r["_hit"] = 0
        write("tools/pub.py", "# per tasks/foo.md\n")  # CITATION-OK
        kept = apply_allowlist(run(), allow)
        expect("(12) an allowlisted violation is suppressed", not kept)
        expect("(12') the live rule counted its hit", allow[0]["_hit"] == 1)
        msgs = []
        stale = _allowlist_arm(allow, msgs.append)
        expect(
            "(12'') a rule matching zero violations is reported stale",
            len(stale) == 1 and stale[0]["id"] == "dead",
        )

        # an allowlist rule with no `why` is refused outright
        alp = os.path.join(tmp, "allow.json")
        with open(alp, "w", encoding="utf-8") as fh:
            json.dump({"rules": [{"id": "x", "sources": ["**"], "targets": ["**"]}]}, fh)
        try:
            load_allowlist(alp)
            expect("(12''') an allowlist rule without a `why` is refused", False)
        except SystemExit:
            expect("(12''') an allowlist rule without a `why` is refused", True)

        # the ratchet: tightening-only
        bl = os.path.join(tmp, "baseline.json")
        update_baseline({"tools/pub.py": 1}, bl)  # CITATION-OK
        expect("ratchet: a baseline written round-trips", load_baseline(bl) == {PUB_PY: 1})
        try:
            update_baseline({"tools/pub.py": 2}, bl)  # CITATION-OK
            expect("ratchet: --update-baseline REFUSES to raise", False)
        except SystemExit:
            expect("ratchet: --update-baseline REFUSES to raise", True)
        update_baseline({"tools/pub.py": 0}, bl)  # CITATION-OK
        expect("ratchet: it does lower", load_baseline(bl) == {"tools/pub.py": 0})  # CITATION-OK
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # ---- 10/11: the REAL tree. The control is phrased against the SCANNER, not against a ledger
    # verdict, so no disposition change can retire it: prepend a synthetic rule withholding
    # docs/architecture.md (a doc the real ledger PUBLISHES and published files legitimately cite)
    # and the same real citations must reappear as PRIVATE.
    led = cp.load_ledger()
    paths = cp.tracked_paths()
    idx = Index(paths)
    real_pub = set(cp.publish_list(paths, led))
    real_v, real_visited = scan(real_pub, idx, include_bare=True, check_disk=False)
    expect(
        "(11) walker liveness: >100 published text files visited in the real tree",
        real_visited > 100,
    )
    expect(
        "(10a) docs/architecture.md is NOT a violation under the real ledger",
        not [v for v in real_v if v["target"] == "docs/architecture.md"],
    )

    probe = {
        "id": "_selftest_probe",
        "disposition": "withhold",
        "globs": ["docs/architecture.md"],
        "why": "selftest probe",
    }
    probe["_res"] = [cp.glob_re(g) for g in probe["globs"]]
    probe_led = dict(led)
    probe_led["rules"] = [probe] + list(led["rules"])
    probe_pub = set(cp.publish_list(paths, probe_led))
    probe_v, _ = scan(probe_pub, idx, include_bare=True, check_disk=False)
    hits = [v for v in probe_v if v["target"] == "docs/architecture.md"]
    expect(
        "(10b) KNOWN-POSITIVE (real tree, synthetic withhold): the real citations of "
        "docs/architecture.md reappear",
        len(hits) >= 5,
    )
    expect(
        "(10c) ...and they are PRIVATE, from real files at real lines",
        bool(hits) and all(v["status"] == PRIVATE and v["line"] > 0 for v in hits),
    )

    for name, passed in results:
        print("  %-68s %s" % (name, "ok" if passed else "FAIL"))
    print("lint_citations --selftest: %s" % ("PASS" if ok else "FAIL"))
    return ok


# --------------------------------------------------------------------------- main


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dangling", action="store_true", help="just the dangling arm")
    ap.add_argument("--prose", action="store_true", help="just Direction A (published .md)")
    ap.add_argument("--code", action="store_true", help="just Direction B (the ratchet)")
    ap.add_argument("--report", action="store_true", help="counts, top sources, top targets")
    ap.add_argument("--strict", action="store_true", help="with --report, also list UNTRACKED")
    ap.add_argument(
        "--update-baseline", action="store_true", help="tighten the Direction B ratchet"
    )
    ap.add_argument("--no-bare", action="store_true", help="drop the bare-basename token class")
    ap.add_argument("--selftest", action="store_true", help="the planted negatives still fire")
    args = ap.parse_args()

    if args.selftest:
        return 0 if selftest() else 1
    if args.report:
        return report(strict=args.strict)

    if args.update_baseline:
        vios, _r, _v, _p = collect(include_bare=not args.no_bare)
        update_baseline(code_counts(vios))
        return 0

    if args.dangling or args.prose or args.code:
        vios, rules, visited, _p = collect(include_bare=not args.no_bare)
        bad = []
        if args.dangling:
            bad += _dangling(vios, print)
        if args.prose:
            bad += _prose(vios, print)
        rc = 0
        if args.code:
            probs, counts = _code(vios, print)
            print("code: %d line(s) over %d file(s)" % (sum(counts.values()), len(counts)))
            rc = 1 if probs else 0
        if args.dangling or args.prose:
            print("%d violation(s)" % len(bad))
            rc = rc or (1 if bad else 0)
        return rc

    return 0 if gate() else 1


if __name__ == "__main__":
    sys.exit(main())
