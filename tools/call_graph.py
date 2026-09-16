#!/usr/bin/env python3
"""call_graph.py -- load the committed call graph, and walk it.

Extracted from derive_write_closure.py at F2D (2026-09-12), when that tool was deleted with the
differential-oracle machinery it served. These two functions were never part of that machinery --
they are a graph loader and a BFS, and three surviving generators depend on them:

    tools/gen_boot_snapshot.py     load_graph + reachable
    tools/gen_notify_readback.py   load_graph
    the seam-surface measurer      load_graph  (research tooling, not carried in every tree)

EXTRACTED RATHER THAN INLINED, and the reason is recorded in the history of load_graph itself. The
graph has THREE edge sources, and a walk that knows only the first is not a slower answer, it is a
wrong one:

  * `links` -- ordinary call references.
  * `tail_jumps[].reaches` -- Watcom shares tails between sibling functions, and a `JMP` into
    another function's BODY produces no call reference, so `links` cannot contain what runs past it.
    Measured, not hypothetical: llm_strat_ai_group_task_recruit_from_pool4 ends `JMP 0x004ea65d`,
    landing on the `CALL llm_strat_ai_group_member_move` it shares with its sibling pool3. A walk
    without tail jumps reported a 6-function closure where the byte-identical sibling got 9, and a
    declaration built on that answer produced a real-looking `ai_groups[4].head_unit` divergence on
    the rig. Each landing point records the callees reachable PAST it -- address-precise, because
    most land in a shared epilogue that reaches nothing, and following the containing FUNCTION would
    over-declare instead.
  * `fallthrough_entries.json` -- measured fall-through edges no reference-derived graph contains.

Three copies of that would be three chances to fork it. One copy is the point.
"""

from __future__ import annotations

import json
from collections import deque
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CALLGRAPH = REPO / "tools" / "data" / "call_graph_no_crt.json"
FALLTHROUGH = REPO / "tools" / "data" / "fallthrough_entries.json"


def load_graph(tail_jumps=True):
    """-> (name_by_id, ids_by_name, succ) over the committed graph's three edge sources."""
    g = json.loads(CALLGRAPH.read_text(encoding="utf-8"))
    name_by_id = {n["id"]: n["name"] for n in g["nodes"]}
    ids_by_name = {}
    for n in g["nodes"]:
        ids_by_name.setdefault(n["name"], []).append(n["id"])
    succ = {}
    for e in g["links"]:
        succ.setdefault(e["source"], set()).add(e["target"])
    # Cross-function JMPs: attach what the LANDING CODE calls, not the containing function. See the
    # module docstring -- the containing function is usually just whoever owns the shared epilogue.
    if tail_jumps:
        for e in g.get("tail_jumps", []):
            for c in e.get("reaches", ()):
                succ.setdefault(e["source"], set()).add(c)
    if FALLTHROUGH.exists():
        ft = json.loads(FALLTHROUGH.read_text(encoding="utf-8"))
        pairs = ft.get("pairs", ft) if isinstance(ft, dict) else ft
        for p in pairs:
            a = (p.get("from") or p.get("src") or "").lower().lstrip("0x")
            b = (p.get("to") or p.get("dst") or "").lower().lstrip("0x")
            if a and b:
                succ.setdefault(a.zfill(8), set()).add(b.zfill(8))
    return name_by_id, ids_by_name, succ


def reachable(root_ids, succ):
    """-> the set of ids reachable from `root_ids` (inclusive)."""
    seen, q = set(root_ids), deque(root_ids)
    while q:
        cur = q.popleft()
        for nxt in succ.get(cur, ()):
            if nxt not in seen:
                seen.add(nxt)
                q.append(nxt)
    return seen
