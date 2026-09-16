#!/usr/bin/env python3
"""gen_hostapi_callers.py -- per-callback caller sets for the host-callback ABI (LIB-ABI-LIFT).

For every entry in libmh_host_api.gen.h + libmh_tact_host_api.gen.h (the two tables since
LIB-IFACE-SPLIT; an entry in both is measured once), measures WHO in libmh depends on it:

  direct      call-graph callers of the entry that are TRANSLATED functions (the original
              EN call graph resolves the per-TU binder-line indirection: a calls-struct
              member bound from mh::host()/mh::tact_host() is used by the TU's functions,
              and those edges exist in the graph under the original names).
  transitive  translated functions reaching the entry through translated-only paths --
              the set that would inherit the dependency in a standalone libmh.
  sites       TUs in src/mh_dll/mh/** referencing mh::host().<name> or
              mh::tact_host().<name> (binder lines count). This is also the relation
              gen_libmh_hostapi.py derives table MEMBERSHIP from.

Feeds the LIFT-* cut decisions (docs/libmh-abi.md): the per-cluster caller sets are what
"cut at the highest sim-write-free node" is measured against, and the per-class fan-in
(sound reaches 7 domains, 74 of 114 entries are single-caller) is the evidence that the
mechanical table sits at the translation frontier.

Callers with no migration-ledger row (the pre-ledger MH_EXPORT/SHADOW installs in
lockstep/save/orders) are attributed as domain "pre:<module>".

Output: tools/data/hostapi_callers.json (stable ordering, callbacks sorted by direct
callers descending). Rerun after a lift lands to watch the surface shrink; entries
converted to channel events leave the generated header and therefore this census.

--md [PATH] additionally renders the human-readable report (main table, per-callback
detail, combined sets per class, per-domain reach) -- the LIB-ABI-LIFT working document;
default PATH is tmp/libmh_abi_lift/hostapi_callers.md.
"""

import io
import json
import os
import re
import sys
from collections import defaultdict, deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _dllsrc  # noqa: E402
import _reimpl  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN_HDRS = (
    os.path.join(REPO, "src", "mh_dll", "libmh", "include", "libmh_host_api.gen.h"),
    os.path.join(REPO, "src", "mh_dll", "libmh", "include", "libmh_tact_host_api.gen.h"),
)
MH_SRC = os.path.join(REPO, "src", "mh_dll", "mh")
OUT_PATH = os.path.join(REPO, "tools", "data", "hostapi_callers.json")

X_RE = re.compile(r'X\((\w+), "([\w-]+)", (\d)\)')
HOST_RE = re.compile(r"\bmh::(?:tact_)?host\(\)\.(\w+)")
LINE_C = re.compile(r"//[^\n]*")
BLOCK_C = re.compile(r"/\*.*?\*/", re.S)


def _strip(text):
    return LINE_C.sub("", BLOCK_C.sub("", text))


def load_entries():
    """[(name, category, notify)] from BOTH generated headers' X-macros, deduped (an entry
    in both tables -- the LIB-IFACE-SPLIT overlap -- is one callback to this census)."""
    entries, seen = [], set()
    for path in GEN_HDRS:
        for m in X_RE.finditer(io.open(path, encoding="utf-8").read()):
            if m.group(1) in seen:
                continue
            seen.add(m.group(1))
            entries.append((m.group(1), m.group(2), m.group(3) == "1"))
    return entries


def load_origin_names():
    """{emitted entry name: the ORIGINAL callee it routes onto}.

    A reshaped entry (a ledger row's `abi` block, SIMABI-VFS) is emitted under a designed name --
    `vfs_open`, not `utils_open_file` -- and the original EN call graph has no such node, so the
    direct/transitive measures below would silently read 0 for the whole io group. The ledger key IS
    the original callee, so this maps back. Identity for every un-reshaped entry."""
    ledger = json.load(
        open(os.path.join(REPO, "tools", "data", "libmh_call_ledger.json"), encoding="utf-8")
    )["callees"]
    origin = {}
    for key, row in ledger.items():
        abi = row.get("abi")
        if isinstance(abi, dict):
            origin[abi["name"]] = key
    return origin


def load_translated_domains():
    """{translated fn name: domain}; pre-ledger installs get 'pre:<module>'."""
    name_domain = {}
    for domain, path in _reimpl._manifests(REPO):
        d = _reimpl._load_json(path)
        if not d:
            continue
        for r in d.get("functions", ()):
            if r.get("state") == _reimpl.DONE_STATE:
                name_domain[r["name"]] = domain
    root = os.path.join(REPO, _reimpl.DLL_SRC)
    for dirpath, _dirnames, filenames in os.walk(root):
        if os.path.basename(dirpath) == "addr":
            continue
        for fn in filenames:
            if not fn.endswith((".cpp", ".h")):
                continue
            p = os.path.join(dirpath, fn)
            text = io.open(p, encoding="utf-8", errors="replace").read()
            for m in _reimpl.RE_REPLACE.finditer(text):
                nm = m.group(1)
                if nm in _reimpl.MACRO_PARAM_NAMES or nm in name_domain:
                    continue
                rel = os.path.relpath(p, root).replace("\\", "/").split("/")
                # F5O: `libmh` joined `mh` as a tree whose SECOND component is the module.
                # Reading only "mh" here collapsed lockstep/orders/save into one "pre:libmh"
                # bucket the moment the roster moved.
                mod = rel[1] if rel[0] in ("mh", "libmh") and len(rel) > 2 else rel[0]
                name_domain[nm] = "pre:" + mod
    return name_domain


def load_graph_preds():
    """{callee name: {caller names}} from the original EN call graph (CRT trimmed)."""
    g = json.load(
        open(os.path.join(REPO, "tools", "data", "call_graph_no_crt.json"), encoding="utf-8")
    )
    id_name = {n["id"]: n["name"] for n in g["nodes"]}
    preds = defaultdict(set)
    for e in g["links"]:
        s, t = id_name.get(e["source"]), id_name.get(e["target"])
        if s and t and s != t:
            preds[t].add(s)
    return preds


def scan_sites(names):
    """{callback: {module: [TU relpaths]}} for mh::host()/mh::tact_host() references."""
    names = set(names)
    sites = defaultdict(lambda: defaultdict(set))
    for tree, root, dirs, files in _dllsrc.walk():
        rel_root = _dllsrc.rel_dir(tree, root)
        top = "" if rel_root == "." else rel_root.replace("\\", "/").split("/", 1)[0]
        if top in ("addr", "attic"):
            dirs[:] = []
            continue
        for fn in files:
            if not fn.endswith((".cpp", ".h")):
                continue
            rel = _dllsrc.rel_or_raise(os.path.join(root, fn))
            mod = rel.split("/", 1)[0] if "/" in rel else "(root)"
            text = _strip(
                io.open(os.path.join(root, fn), encoding="utf-8", errors="replace").read()
            )
            for cb in set(HOST_RE.findall(text)):
                if cb in names:
                    sites[cb][mod].add(rel)
    return sites


def transitive_callers(cb, preds, translated):
    """Translated fns reaching cb through translated-only intermediate nodes."""
    seen = set(p for p in preds.get(cb, ()) if p in translated)
    q = deque(seen)
    while q:
        f = q.popleft()
        for p in preds.get(f, ()):
            if p in translated and p not in seen:
                seen.add(p)
                q.append(p)
    return seen


def main():
    entries = load_entries()
    name_domain = load_translated_domains()
    translated = set(name_domain)
    preds = load_graph_preds()
    sites = scan_sites([n for n, _c, _v in entries])
    origin = load_origin_names()

    rows = []
    for name, cat, notify in entries:
        graph_name = origin.get(name, name)  # a reshaped entry measures under its ORIGINAL callee
        direct = sorted(p for p in preds.get(graph_name, ()) if p in translated)
        trans = transitive_callers(graph_name, preds, translated)
        dom_direct = defaultdict(list)
        for c in direct:
            dom_direct[name_domain[c]].append(c)
        dom_trans = defaultdict(int)
        for c in trans:
            dom_trans[name_domain[c]] += 1
        rows.append(
            {
                "name": name,
                "category": cat,
                "notify": notify,
                "n_direct": len(direct),
                "n_transitive": len(trans),
                "direct_by_domain": {k: sorted(v) for k, v in sorted(dom_direct.items())},
                "transitive_by_domain": dict(sorted(dom_trans.items())),
                "site_modules": {m: sorted(v) for m, v in sorted(sites[name].items())},
            }
        )
    rows.sort(key=lambda r: (-r["n_direct"], -r["n_transitive"], r["name"]))

    by_domain = defaultdict(lambda: {"direct": set(), "transitive": set()})
    by_class = defaultdict(lambda: {"callbacks": 0, "callers": set()})
    for r in rows:
        by_class[r["category"]]["callbacks"] += 1
        for d, fns in r["direct_by_domain"].items():
            by_domain[d]["direct"].add(r["name"])
            by_class[r["category"]]["callers"].update(fns)
        for d in r["transitive_by_domain"]:
            by_domain[d]["transitive"].add(r["name"])

    out = {
        "_generated_by": "tools/gen_hostapi_callers.py -- do not hand-edit; regenerate instead",
        "entry_count": len(entries),
        "rows": rows,
        "by_domain": {
            k: {
                "n_direct": len(v["direct"]),
                "n_transitive": len(v["transitive"]),
                "direct": sorted(v["direct"]),
                "transitive": sorted(v["transitive"]),
            }
            for k, v in sorted(by_domain.items())
        },
        "by_class": {
            k: {"n_callbacks": v["callbacks"], "n_distinct_direct_callers": len(v["callers"])}
            for k, v in sorted(by_class.items())
        },
    }
    if "--check" in sys.argv:
        # Drift gate (lint_repo): a membership slice that forgets to regenerate leaves the
        # caller evidence describing a table that no longer exists (bit twice, 2026-09-10).
        try:
            on_disk = json.load(open(OUT_PATH, encoding="utf-8"))
        except (OSError, ValueError):
            on_disk = None
        if on_disk != out:
            print(
                "[gen_hostapi_callers] FAIL: %s is stale -- rerun tools/gen_hostapi_callers.py"
                % os.path.relpath(OUT_PATH, REPO),
                file=sys.stderr,
            )
            sys.exit(1)
        print("[gen_hostapi_callers] OK: caller evidence current (%d entries)" % len(entries))
        return
    json.dump(out, open(OUT_PATH, "w", encoding="utf-8"), indent=1, sort_keys=False)
    print(
        "[gen_hostapi_callers] %d entries; zero-direct-caller entries: %s"
        % (len(entries), [r["name"] for r in rows if r["n_direct"] == 0] or "none")
    )
    for k, v in out["by_class"].items():
        print(
            "  %-14s callbacks %3d  distinct direct callers %3d"
            % (k, v["n_callbacks"], v["n_distinct_direct_callers"])
        )
    print("wrote", os.path.relpath(OUT_PATH, REPO))

    if "--md" in sys.argv:
        i = sys.argv.index("--md")
        md_path = (
            sys.argv[i + 1]
            if i + 1 < len(sys.argv) and not sys.argv[i + 1].startswith("-")
            else os.path.join(REPO, "tmp", "libmh_abi_lift", "hostapi_callers.md")
        )
        os.makedirs(os.path.dirname(md_path), exist_ok=True)
        io.open(md_path, "w", encoding="utf-8", newline="\n").write(render_md(out))
        print("wrote", os.path.relpath(md_path, REPO))


def render_md(out):
    """The LIB-ABI-LIFT caller-set report, rendered from the census dict."""
    rows = out["rows"]
    L = []
    L.append("# LIB-ABI-LIFT input: host-callback caller sets\n")
    L.append(
        "Generated by tools/gen_hostapi_callers.py (--md) from libmh_host_api.gen.h "
        "(%d entries), call_graph_no_crt.json (original EN call graph), the migration "
        "manifests (translated set + domain), the pre-ledger MH_EXPORT/SHADOW scan "
        "(domain = `pre:<module>`), and the mh::host()/mh::tact_host() source sites.\n"
        % out["entry_count"]
    )
    L.append(
        "- **direct** = call-graph callers of the entry that are translated libmh functions "
        "(first degree).\n"
        "- **transitive** = translated functions reaching the entry through translated-only "
        "paths.\n"
        "- **sites** = TUs in src/mh_dll/mh/** referencing `mh::host().<name>` or "
        "`mh::tact_host().<name>` (binder lines included).\n"
    )
    L.append("## Callbacks by direct-caller count (descending)\n")
    L.append("| # | callback | class | direct | transitive | direct domains | site TUs |")
    L.append("|--:|---|---|--:|--:|---|---|")
    for i, r in enumerate(rows, 1):
        doms = ", ".join("%s:%d" % (d, len(v)) for d, v in r["direct_by_domain"].items())
        site = ", ".join("%s(%d)" % (m, len(v)) for m, v in r["site_modules"].items())
        L.append(
            "| %d | `%s` | %s | %d | %d | %s | %s |"
            % (i, r["name"], r["category"], r["n_direct"], r["n_transitive"], doms, site)
        )
    L.append("")
    L.append("## Per-callback detail\n")
    for r in rows:
        L.append(
            "### `%s` (%s, direct %d, transitive %d)\n"
            % (r["name"], r["category"], r["n_direct"], r["n_transitive"])
        )
        for d, fns in r["direct_by_domain"].items():
            L.append("- **%s** direct: %s" % (d, ", ".join("`%s`" % f for f in fns)))
        if r["transitive_by_domain"]:
            L.append(
                "- transitive by domain: %s"
                % ", ".join("%s:%d" % (d, n) for d, n in r["transitive_by_domain"].items())
            )
        sites = sorted(p for v in r["site_modules"].values() for p in v)
        if sites:
            L.append("- sites: %s" % ", ".join("`%s`" % p for p in sites))
        L.append("")
    L.append("## Combined sets per class\n")
    classes = {}
    for r in rows:
        c = classes.setdefault(r["category"], {"n": 0, "callers": set(), "dd": set(), "td": set()})
        c["n"] += 1
        for d, fns in r["direct_by_domain"].items():
            c["dd"].add(d)
            c["callers"].update(fns)
        c["td"].update(r["transitive_by_domain"])
    for cat, c in sorted(classes.items()):
        L.append(
            "### %s (%d callbacks, %d distinct direct callers)\n" % (cat, c["n"], len(c["callers"]))
        )
        L.append("- domains (direct): %s" % ", ".join(sorted(c["dd"])))
        L.append("- domains (transitive): %s" % ", ".join(sorted(c["td"] | c["dd"])))
        L.append(
            "- combined direct caller set: %s" % ", ".join("`%s`" % f for f in sorted(c["callers"]))
        )
        L.append("")
    L.append("## Per-domain callback reach\n")
    doms = {}
    for r in rows:
        for d, fns in r["direct_by_domain"].items():
            doms.setdefault(d, {"direct": set(), "trans": set()})["direct"].add(r["name"])
        for d in r["transitive_by_domain"]:
            doms.setdefault(d, {"direct": set(), "trans": set()})["trans"].add(r["name"])
    for d, v in sorted(doms.items(), key=lambda kv: -len(kv[1]["trans"] | kv[1]["direct"])):
        trans_only = sorted(v["trans"] - v["direct"])
        L.append(
            "### %s (direct %d, transitive %d)\n"
            % (d, len(v["direct"]), len(v["trans"] | v["direct"]))
        )
        L.append("- direct: %s" % (", ".join("`%s`" % f for f in sorted(v["direct"])) or "(none)"))
        if trans_only:
            L.append("- transitive-only: %s" % ", ".join("`%s`" % f for f in trans_only))
        L.append("")
    return "\n".join(L) + "\n"


if __name__ == "__main__":
    main()
