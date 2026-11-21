"""Invention dependency graph -> layered layout -> SVG tech tree (wiki, mods:E3).

Pure Python, deterministic (every iteration is over sorted keys, floats are
formatted with fixed precision). No Graphviz.

Data model (retail cfg):
  * ``PROGRESS`` records are the inventions (286 in retail EN). ``depends`` is the
    prerequisite list; the game unlocks an invention when EVERY prerequisite is
    acquired (AND, tracker mods:R2 / ``llm_progress_propagate_unlocks``).
  * Each invention is owned by one UNIT / BUILDING / PROJECT / UPGRADE / PLANET /
    SYSTEM record through that record's ``invention`` field.
  * Faction is the second name token: ``V_alien_*`` / ``V_human_*``. ``V_planeta_*``
    and ``V_uklad*`` (planet / star-system gates) are SHARED and are drawn in
    each faction's tree when something in it requires them (transitively).
  * A self-dependency (``V_alien_Matka`` needs itself) marks a start-of-game
    invention; the self edge is dropped and reported in ``graph["issues"]``.

API:
    load_model(game)                           -> cfgkit Model (retail dir/zip)
    build_graph(model, labels=None)            -> graph
    faction_graph(graph, faction)              -> sub-graph (shared gates pulled in)
    layout(graph)                              -> layout dict (positions, edges, stats)
    render_svg(graph, lay, icon_map, labels)   -> svg text
    render_page(graph, lay, faction, ...)      -> html text (Jinja2 template)
    check_graph(graph, lay, svg) / selftest()  -> list of problems / raises
"""

from __future__ import annotations

import json
import os
import re
from xml.sax.saxutils import escape, quoteattr

HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------- constants
NW, NH = 176, 46  # node box
GAP_X = 74  # gap between rank columns
MARGIN = 24
ROW_GAP = 12  # min vertical gap between two real nodes
DUMMY_H = 6
DUMMY_GAP = 4
ICON = 34  # icon square inside a node
SWEEPS = 4

KIND_OF_CLASS = {
    "UNIT": "unit",
    "BUILDING": "building",
    "PROJECT": "project",
    "UPGRADE": "upgrade",
    "PLANET": "planet",
    "SYSTEM": "system",
}
KIND_GLYPH = {
    "unit": "U",
    "building": "B",
    "project": "P",
    "upgrade": "Up",
    "planet": "Pl",
    "system": "Sy",
    "none": "?",
}
CLASS_DIR = {
    "unit": "units",
    "building": "buildings",
    "project": "projects",
    "planet": "planets",
    "system": "systems",
}


# ---------------------------------------------------------------- loading
def load_model(game, lang_pack="mh_ex"):
    """Parse INIT.CFG (pack ``mh``) + INITLANG.CFG (``mh_ex``) from a retail dir/zip.

    ``game`` is a directory or .zip accepted by ``fnt.Source``. Only the ``init\\``
    members are decompressed (the whole-pack ``all_members`` call takes ~100 s).
    """
    from .. import fnt
    from ..cfgkit.parse import parse_text

    src = fnt.Source(game)

    def cfg(pack, leaf):
        for k, v in src.members(pack, prefix="init\\").items():
            if k.lower().endswith(leaf):
                return v.decode("utf-16-le").lstrip("﻿")
        raise FileNotFoundError(leaf + " not in pack " + pack)

    return parse_text(cfg("mh", "init.cfg") + "\n" + cfg(lang_pack, "initlang.cfg"))


# ---------------------------------------------------------------- graph
def _faction_of(name):
    parts = name.split("_")
    if len(parts) > 1 and parts[1] in ("alien", "human"):
        return parts[1]
    return "shared"


def _pretty(owner):
    """N_alien_pr_Statek_up1 -> 'pr Statek up1' (fallback label when no TEXT)."""
    parts = owner.split("_")
    if parts and parts[0] in ("N", "V"):
        parts = parts[1:]
    if parts and parts[0] in ("alien", "human"):
        parts = parts[1:]
    return " ".join(parts) or owner


def _card(cls, rec):
    """Raw cfg stats for the hover card (labels are cfg keywords; a model may override)."""
    rows = []

    def add(k, v):
        if v not in (None, ""):
            rows.append([k, v])

    if cls == "UNIT":
        add("ENERGY", rec.get("energy"))
        add("ARMOR", rec.get("armor"))
        add("SIGHT", rec.get("sight"))
        add("BUILD_TIME", round(rec.get("build_time", 0), 2))
    elif cls == "BUILDING":
        add("ENERGY", rec.get("energy"))
        add("ELECTRIC_POWER", rec.get("electric_power"))
        add("SIGHT", rec.get("sight"))
        add("BUILD_TIME", round(rec.get("build_time", 0), 2))
    elif cls == "PROJECT":
        add("BUILD_TIME", round(rec.get("build_time", 0), 2))
        if rec.get("resources"):
            add("RESOURCE", ", ".join(f"{a} {b}" for a, b in rec["resources"]))
    elif cls == "UPGRADE":
        add("TYPE", rec.get("type"))
        n = len(rec.get("objects") or {})
        if n:
            add("OBJECTS", n)
    return rows


# BUILDING TYPE role (alien 1..18, human 21..38 -> 1..18): 14 = main base, 18 = civil building.
# Neither can be constructed in a normal game (no RESOURCE cost, no BUILD_TIME, no construction
# animation in their cfg records; user review 2026-10-26), so the tech tree leaves them out.
# Their wiki pages stay.  Keep in step with ``model.UNBUILDABLE_ROLES``.
UNBUILDABLE_ROLES = {14: "main base", 18: "civil building"}


def _building_role(model, rec):
    t = dict(getattr(model, "defines", []) or []).get(rec.get("type", ""), 0)
    return t - 20 if t >= 20 else t


# Multi-soldier squads (ZALOGA2..5 unit types) are formed by merging single soldiers on the field;
# nobody can produce them directly, so the tree leaves them out (their pages stay).
SQUAD_MIN = 2
PORT_ROLE = 12  # BUILDING TYPE role of the spaceport (alien 12, human 32 -> 12)


def _squad_size(rec):
    m = re.search(r"ZALOGA(\d)", rec.get("type", ""))
    return int(m.group(1)) if m else 0


def _derive_port_edges(model, nodes):
    """Spaceport -> the unit it launches (cfg ``EQUIVALENT``): the pod / transport rocket is spawned
    by the spaceport only, but the cfg gives it no dependency on the port."""
    port_of = {}
    for rec in model.classes.get("BUILDING", []):
        if _building_role(model, rec) == PORT_ROLE and rec.get("invention") in nodes:
            port_of[rec["__name__"]] = rec["invention"]
    derived = set()
    for rec in model.classes.get("UNIT", []):
        port = port_of.get(rec.get("equivalent", ""))
        if port and rec.get("invention") in nodes:
            derived.add((port, rec["invention"]))
    return derived


def _derive_system_edges(model, nodes, issues):
    """Game rules the cfg does not state as DEPEND edges (returned as {(pre, dep)}).

    * a system's "conquered" invention needs EVERY planet of the system conquered (the game grants
      the system invention when the last enemy of the system is cleared, ``K_UKLAD_ZDOBYTY``);
    * a later system's entry planets (planets with no prerequisite inside their own system) need
      the previous system conquered (the cfg only chains the system inventions themselves).
    """
    derived = set()
    systems = model.classes.get("SYSTEM", [])
    by_name = {r["__name__"]: r for r in model.classes.get("PLANET", [])}
    sys_inv = [r.get("invention") for r in systems]
    for k, srec in enumerate(systems):
        sinv = srec.get("invention")
        if sinv not in nodes:
            continue
        planets = []
        for slot in sorted(srec.get("planets", {})):
            prec = by_name.get(srec["planets"][slot])
            if prec is not None and prec.get("invention") in nodes:
                planets.append(prec["invention"])
        for pi in planets:
            derived.add((pi, sinv))
        prev = None
        for d in nodes[sinv]["deps"]:  # the cfg already chains system -> previous system
            if d in sys_inv and d != sinv:
                prev = d
        if prev is None and k > 0 and sys_inv[k - 1] in nodes:
            prev = sys_inv[k - 1]
        if prev is not None:
            mine = set(planets)
            for pi in planets:
                if not (set(nodes[pi]["deps"]) & mine):
                    derived.add((prev, pi))
    derived |= _derive_port_edges(model, nodes)
    new = set()
    for a, b in sorted(derived):
        if a == b:
            continue
        if a in nodes[b]["deps"]:
            issues.append(f"game rule edge {a} -> {b} is already in the config")
        else:
            nodes[b]["deps"].append(a)
            nodes[b]["deps"].sort()
            new.add((a, b))
    return new


def build_graph(model, labels=None, drop_unbuildable=True):
    """Return ``{"nodes": {id: node}, "edges": [(pre, id)], "derived": [(pre, id)],
    "dropped": {id: reason}, "issues": [...]}``.

    ``derived`` are edges the game enforces in code but the cfg does not list (planet -> system,
    previous system -> entry planet, spaceport -> pod / rocket); they are part of ``edges`` and ``node["deps"]`` too.
    ``dropped`` are inventions nobody can produce directly (main base, civil buildings, see
    ``UNBUILDABLE_ROLES``; multi-soldier squads, see ``SQUAD_MIN``); they are not nodes.

    node = id, faction, kind, owner_class, owner, label, type, deps (sorted
    unique prerequisites, self edge removed), card, start (self-dependent).
    ``labels`` optionally overrides labels: {invention id: text}.
    """
    labels = labels or {}
    texts = dict(model.texts)
    owners = {}
    for cls in sorted(KIND_OF_CLASS):
        for rec in model.classes.get(cls, []):
            inv = rec.get("invention")
            if inv and inv not in owners:
                owners[inv] = (cls, rec)
    nodes, issues = {}, []
    for rec in model.classes["PROGRESS"]:
        inv = rec["__name__"]
        if inv in nodes:
            issues.append(f"duplicate invention {inv}")
            continue
        cls, orec = owners.get(inv, (None, None))
        owner = orec["__name__"] if orec else ""
        label = labels.get(inv) or texts.get(owner) or _pretty(owner or inv)
        deps, start = [], False
        for d in rec.get("depends", []):
            if d == inv:
                start = True
                issues.append(f"self-dependency {inv} (treated as a start invention)")
            elif d not in deps:
                deps.append(d)
        nodes[inv] = {
            "id": inv,
            "faction": _faction_of(inv),
            "kind": KIND_OF_CLASS.get(cls, "none"),
            "owner_class": cls or "",
            "owner": owner,
            "label": label,
            "type": rec.get("type", ""),
            "deps": sorted(deps),
            "card": _card(cls, orec) if orec else [],
            "start": start,
            "dd": [],
        }
        if cls == "BUILDING" and _building_role(model, orec) in UNBUILDABLE_ROLES:
            nodes[inv]["unbuildable"] = UNBUILDABLE_ROLES[_building_role(model, orec)]
        if cls == "UNIT" and _squad_size(orec) >= SQUAD_MIN:
            nodes[inv]["unbuildable"] = f"squad of {_squad_size(orec)}"
        if not orec:
            issues.append(f"invention {inv} has no owning object")
    derived = _derive_system_edges(model, nodes, issues)
    dropped = {}
    if drop_unbuildable:
        asked = {i: n["unbuildable"] for i, n in nodes.items() if n.get("unbuildable")}
        for i, why in sorted(asked.items()):
            users = sorted(j for j, n in nodes.items() if i in n["deps"] and j not in asked)
            if users:
                issues.append(f"{i} ({why}) kept: {users[0]} needs it")
                continue
            dropped[i] = why
        for i in dropped:
            del nodes[i]
        for n in nodes.values():
            n["deps"] = [d for d in n["deps"] if d not in dropped]
    for a, b in derived:
        if b in nodes and a in nodes:
            nodes[b]["dd"].append(a)
    for n in nodes.values():
        n["dd"] = sorted(set(n["dd"]))
    edges = []
    for inv in sorted(nodes):
        kept = []
        for d in nodes[inv]["deps"]:
            if d in nodes:
                edges.append((d, inv))
                kept.append(d)
            else:
                issues.append(f"dangling prerequisite {d} -> {inv}")
        nodes[inv]["deps"] = kept
    keep_d = sorted(e for e in derived if e in set(edges))
    return {
        "nodes": nodes,
        "edges": edges,
        "derived": keep_d,
        "dropped": dropped,
        "issues": issues,
    }


def faction_graph(graph, faction):
    """Nodes of ``faction`` plus every shared planet / system gate."""
    nodes = graph["nodes"]
    # the shared planet / system gates are all drawn: system N needs every planet of the system,
    # even when no faction invention waits for that system
    keep = {i for i, n in nodes.items() if n["faction"] in (faction, "shared")}
    todo = sorted(keep)
    while todo:
        cur = todo.pop()
        for d in nodes[cur]["deps"]:
            if d not in keep and nodes[d]["faction"] == "shared":
                keep.add(d)
                todo.append(d)
    sub = {i: nodes[i] for i in sorted(keep)}
    edges = [(a, b) for a, b in graph["edges"] if a in keep and b in keep]
    stray = [e for e in graph["edges"] if e[1] in keep and e[0] not in keep]
    issues = ["edge leaves faction tree: {} -> {}".format(*e) for e in stray]
    es = set(edges)
    derived = [e for e in graph.get("derived", []) if e in es]
    return {
        "nodes": sub,
        "edges": edges,
        "derived": derived,
        "dropped": graph.get("dropped", {}),
        "issues": issues,
        "faction": faction,
    }


def find_cycles(graph):
    """Return the node ids left over after Kahn's algorithm (empty if acyclic)."""
    indeg = {i: 0 for i in graph["nodes"]}
    succ = {i: [] for i in graph["nodes"]}
    for a, b in graph["edges"]:
        indeg[b] += 1
        succ[a].append(b)
    ready = sorted(i for i, d in indeg.items() if d == 0)
    while ready:
        cur = ready.pop()
        for s in succ[cur]:
            indeg[s] -= 1
            if indeg[s] == 0:
                ready.append(s)
    return sorted(i for i, d in indeg.items() if d > 0)


def _acyclic_edges(graph):
    """Edges with DFS back edges removed (deterministic); returns (edges, removed)."""
    succ = {i: [] for i in graph["nodes"]}
    for a, b in graph["edges"]:
        succ[a].append(b)
    state, back = {}, set()
    for root in sorted(graph["nodes"]):
        if root in state:
            continue
        stack = [(root, iter(sorted(succ[root])))]
        state[root] = 1
        while stack:
            cur, it = stack[-1]
            for s in it:
                if state.get(s) == 1:
                    back.add((cur, s))
                elif s not in state:
                    state[s] = 1
                    stack.append((s, iter(sorted(succ[s]))))
                    break
            else:
                state[cur] = 2
                stack.pop()
    return [e for e in graph["edges"] if e not in back], sorted(back)


# ---------------------------------------------------------------- layout
def _count_crossings(layers, segs):
    """Crossings between adjacent layers. segs[r] = [(upper_vertex, lower_vertex)]."""
    pos = [{v: i for i, v in enumerate(layer)} for layer in layers]
    total = 0
    for r, sl in enumerate(segs):
        pts = [(pos[r][a], pos[r + 1][b]) for a, b in sl]
        for i, (ai, bi) in enumerate(pts):
            for aj, bj in pts[i + 1 :]:
                if (ai - aj) * (bi - bj) < 0:
                    total += 1
    return total


def _isotonic(d, seps):
    """Min sum (y-d)^2 s.t. y[i]-y[i-1] >= seps[i] (pool-adjacent-violators)."""
    cum, c = [], 0.0
    for i, s in enumerate(seps):
        c += s if i else 0.0
        cum.append(c)
    blocks = []  # [sum, count]
    for i, v in enumerate(d):
        blocks.append([v - cum[i], 1])
        while len(blocks) > 1 and blocks[-2][0] / blocks[-2][1] > blocks[-1][0] / blocks[-1][1]:
            s, n = blocks.pop()
            blocks[-1][0] += s
            blocks[-1][1] += n
    out = []
    for s, n in blocks:
        out.extend([s / n] * n)
    return [out[i] + cum[i] for i in range(len(d))]


def _reduce(edges, rank):
    """Transitive reduction of a DAG: drop (a, b) when another path a -> ... -> b exists."""
    succ = {}
    for a, b in edges:
        succ.setdefault(a, []).append(b)
    desc = {}
    for u in sorted(rank, key=lambda k: (-rank[k], k)):
        d = set()
        for w in succ.get(u, []):
            d.add(w)
            d |= desc[w]
        desc[u] = d
    keep = []
    for a, b in edges:
        if not any(b in desc[w] for w in succ[a] if w != b):
            keep.append((a, b))
    return keep


def layout(graph, reduce=True):
    """Layered layout (rank = longest path from roots, left to right).

    Returns a dict: pos {id: (x, y, rank)} (top-left of the node box), edges
    {(a, b): [(x, y), ...]} (anchor points: source right edge, dummies, target
    left edge), width, height, ranks, layers, back_edges, stats.

    reduce=True draws only the transitive reduction (an edge implied by a longer
    path adds nothing to the AND rule); ranks use all edges either way, and the
    node data / JS closures still use the full prerequisite lists.
    """
    nodes = graph["nodes"]
    acyc, back = _acyclic_edges(graph)
    preds = {i: [] for i in nodes}
    for a, b in acyc:
        preds[b].append(a)
    rank = {}
    for i in sorted(nodes):
        stack = [i]
        while stack:  # iterative longest path on the DAG
            cur = stack[-1]
            if cur in rank:
                stack.pop()
                continue
            pend = [p for p in preds[cur] if p not in rank]
            if pend:
                stack.extend(sorted(pend))
            else:
                rank[cur] = 1 + max((rank[p] for p in preds[cur]), default=-1)
                stack.pop()
    nrank = (max(rank.values()) + 1) if rank else 0
    drawn = _reduce(acyc, rank) if reduce else acyc

    # layers, with dummy vertices for edges spanning more than one rank
    layers = [[] for _ in range(nrank)]
    for i in sorted(nodes):
        layers[rank[i]].append(i)
    chains = {}
    segs = [[] for _ in range(max(nrank - 1, 0))]
    for a, b in sorted(drawn):
        chain = [a]
        for r in range(rank[a] + 1, rank[b]):
            d = f"~{a}>{b}~{r}"
            layers[r].append(d)
            chain.append(d)
        chain.append(b)
        chains[(a, b)] = chain
        for k in range(len(chain) - 1):
            segs[rank[a] + k].append((chain[k], chain[k + 1]))
    for layer in layers:
        layer.sort()  # initial order: by id

    up, down = {}, {}
    for sl in segs:
        for u, v in sl:
            down.setdefault(u, []).append(v)
            up.setdefault(v, []).append(u)

    before = _count_crossings(layers, segs)
    best_layers, best = [list(x) for x in layers], before
    for sweep in range(SWEEPS):
        down_sweep = sweep % 2 == 0
        rng = range(1, nrank) if down_sweep else range(nrank - 2, -1, -1)
        nb = up if down_sweep else down
        for r in rng:
            ref = layers[r - 1] if down_sweep else layers[r + 1]
            rpos = {v: i for i, v in enumerate(ref)}
            cur = {v: i for i, v in enumerate(layers[r])}

            def key(v, cur=cur, rpos=rpos, nb=nb):
                ns = nb.get(v)
                bary = sum(rpos[n] for n in ns) / len(ns) if ns else cur[v]
                return (bary, cur[v], v)

            layers[r] = sorted(layers[r], key=key)
        c = _count_crossings(layers, segs)
        if c < best:
            best, best_layers = c, [list(x) for x in layers]
    layers = best_layers

    def hgt(v):
        return DUMMY_H if v.startswith("~") else NH

    ys = {}
    for layer in layers:
        y = 0.0
        for v in layer:
            ys[v] = y
            y += hgt(v) + (DUMMY_GAP if v.startswith("~") else ROW_GAP)
    nbrs = {}
    for sl in segs:
        for u, v in sl:
            nbrs.setdefault(u, []).append(v)
            nbrs.setdefault(v, []).append(u)
    for it in range(8):  # alternate sweeps toward the neighbour mean, order kept
        for r in range(nrank) if it % 2 == 0 else range(nrank - 1, -1, -1):
            layer = layers[r]
            if not layer:
                continue
            want, seps = [], []
            for k, v in enumerate(layer):
                ns = nbrs.get(v)
                want.append(sum(ys[n] for n in ns) / len(ns) if ns else ys[v])
                if k == 0:
                    seps.append(0.0)
                else:
                    p = layer[k - 1]
                    gap = DUMMY_GAP if (v.startswith("~") or p.startswith("~")) else ROW_GAP
                    seps.append((hgt(p) + hgt(v)) / 2 + gap)
            for v, y in zip(layer, _isotonic(want, seps)):
                ys[v] = y
    top = min((ys[v] - hgt(v) / 2 for v in ys), default=0.0)
    for v in ys:
        ys[v] += MARGIN - top  # ys = vertex centre

    pos, cx = {}, {}
    for r, layer in enumerate(layers):
        xl = MARGIN + r * (NW + GAP_X)
        for v in layer:
            cx[v] = xl + NW / 2
            if not v.startswith("~"):
                pos[v] = (xl, ys[v] - NH / 2, r)
    epts = {}
    for (a, b), chain in sorted(chains.items()):
        pts = [(pos[a][0] + NW, ys[a])]
        pts += [(cx[d], ys[d]) for d in chain[1:-1]]
        pts.append((pos[b][0], ys[b]))
        epts[(a, b)] = pts
    height = max((p[1] + NH for p in pos.values()), default=0) + MARGIN
    width = MARGIN * 2 + nrank * NW + max(nrank - 1, 0) * GAP_X
    return {
        "pos": pos,
        "edges": epts,
        "width": width,
        "height": height,
        "ranks": nrank,
        "layers": [[v for v in layer if not v.startswith("~")] for layer in layers],
        "back_edges": back,
        "hidden_edges": sorted(set(acyc) - set(drawn)),
        "stats": {
            "crossings_before": before,
            "crossings_after": best,
            "back_edges": len(back),
            "edges_drawn": len(drawn),
            "dummies": sum(1 for v in ys if v.startswith("~")),
        },
    }


# ---------------------------------------------------------------- SVG
def _f(x):
    return f"{x:.1f}"


def _edge_path(pts):
    d = f"M{_f(pts[0][0])} {_f(pts[0][1])}"
    for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
        mx = (x1 + x2) / 2
        d += f" C{_f(mx)} {_f(y1)} {_f(mx)} {_f(y2)} {_f(x2)} {_f(y2)}"
    return d


def _wrap(text, width=20, lines=2):
    out, cur = [], ""
    for w in text.split():
        if cur and len(cur) + 1 + len(w) > width:
            out.append(cur)
            cur = w
        else:
            cur = (cur + " " + w) if cur else w
    if cur:
        out.append(cur)
    if len(out) > lines:
        out = out[: lines - 1] + [" ".join(out[lines - 1 :])]
    return [(s if len(s) <= width + 2 else s[: width - 1] + "…") for s in out]


def render_svg(graph, lay, icon_map=None, lang_labels=None):
    """SVG for ``graph`` laid out by ``layout``.

    icon_map    {invention id: image href}; missing -> text-only box.
    lang_labels {invention id: text}; overrides the node label.
    """
    icon_map = icon_map or {}
    lang_labels = lang_labels or {}
    nodes = graph["nodes"]
    out = [
        '<svg class="tt-svg" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {} {}" width="{}" height="{}"'
        ' role="group" aria-label="Tech tree">'.format(
            _f(lay["width"]), _f(lay["height"]), _f(lay["width"]), _f(lay["height"])
        )
    ]
    out.append('<g class="tt-edges">')
    derived = set(graph.get("derived", []))
    for (a, b), pts in sorted(lay["edges"].items()):
        gate = " gate" if nodes[a]["faction"] == "shared" else ""
        gate += " derived" if (a, b) in derived else ""
        out.append(
            f'<path class="tt-edge{gate}" data-from={quoteattr(a)} data-to={quoteattr(b)} d="{_edge_path(pts)}"/>'
        )
    out.append("</g>")
    out.append('<g class="tt-nodes">')
    for i in sorted(lay["pos"], key=lambda k: (lay["pos"][k][2], lay["pos"][k][1], k)):
        x, y, r = lay["pos"][i]
        n = nodes[i]
        label = lang_labels.get(i) or n["label"]
        icon = icon_map.get(i)
        tx = x + (ICON + 18 if icon else 14)
        cls = "tt-node k-" + n["kind"] + (" start" if n["start"] else "")
        out.append(
            f'<g class="{cls}" data-id={quoteattr(i)} data-kind={quoteattr(n["kind"])} data-rank="{r}"'
            f' tabindex="0" role="button" aria-label={quoteattr(label)}>'
        )
        out.append(
            f'<rect class="tt-box" x="{_f(x)}" y="{_f(y)}" width="{NW}" height="{NH}" rx="6"/>'
        )
        out.append(
            f'<rect class="tt-stripe" x="{_f(x)}" y="{_f(y)}" width="5" height="{NH}" rx="2"/>'
        )
        if icon:
            out.append(
                f'<image class="tt-icon" x="{_f(x + 10)}" y="{_f(y + (NH - ICON) / 2)}"'
                f' width="{ICON}" height="{ICON}" href={quoteattr(icon)}/>'
            )
        lines = _wrap(label, 18 if icon else 22)
        ty = y + NH / 2 - (len(lines) - 1) * 6.5 + 4
        for k, ln in enumerate(lines):
            out.append(
                f'<text class="tt-label" x="{_f(tx)}" y="{_f(ty + k * 13)}">{escape(ln)}</text>'
            )
        out.append(
            '<text class="tt-glyph" x="{}" y="{}" text-anchor="end">{}</text>'.format(
                _f(x + NW - 6), _f(y + 12), escape(KIND_GLYPH[n["kind"]])
            )
        )
        out.append("</g>")
    out.append("</g></svg>")
    return "\n".join(out)


def page_data(graph, hrefs=None, lang_labels=None, cards=None):
    """JSON-able data for the JS: {id: {l,k,o,h,d,c,s}} (d = direct prerequisites)."""
    hrefs, lang_labels, cards = hrefs or {}, lang_labels or {}, cards or {}
    data = {}
    for i, n in sorted(graph["nodes"].items()):
        data[i] = {
            "l": lang_labels.get(i) or n["label"],
            "k": n["kind"],
            "o": n["owner"],
            "h": hrefs.get(i, ""),
            "d": list(n["deps"]),
            "c": cards.get(i, n["card"]),
            "s": 1 if n["start"] else 0,
            "x": list(n.get("dd", [])),
        }
    return data


def default_hrefs(graph, prefix="../"):
    """Object-page hrefs by the design slug rule (cfg name lower-cased, N_ dropped)."""
    out = {}
    for i, n in graph["nodes"].items():
        d = CLASS_DIR.get(n["kind"])
        if d and n["owner"]:
            slug = n["owner"].lower()
            slug = slug.removeprefix("n_")
            out[i] = f"{prefix}{d}/{slug}.html"
    return out


def _read(rel):
    with open(os.path.join(HERE, rel), encoding="utf-8") as fh:
        return fh.read()


UI_DEFAULT = {
    "unit": "Unit",
    "building": "Building",
    "project": "Project",
    "upgrade": "Upgrade",
    "planet": "Planet gate",
    "system": "System gate",
    "legend": "Legend",
    "rule": "A node unlocks only when ALL of its prerequisites are acquired (left to right).",
    "help": "Click a node to highlight its prerequisite chain and what it unlocks. Click empty space to clear.",
    "requires": "Requires (all of)",
    "unlocks": "Unlocks",
    "open": "Open page",
    "start": "Start-of-game invention",
    "fit": "Fit",
    "zoom_in": "+",
    "zoom_out": "-",
    "gate": "dashed line = planet / system gate",
    "derived": "dotted line = game rule, not in config",
    "none": "nothing",
    "n_nodes": "inventions",
    "n_links": "links",
    "n_ranks": "ranks",
}


def render_page(
    graph,
    lay,
    faction,
    icon_map=None,
    lang_labels=None,
    hrefs=None,
    cards=None,
    title=None,
    ui=None,
    standalone=True,
    asset_prefix="../../assets/",
):
    """Render templates/techtree.html.

    standalone=True inlines the CSS and JS into a full page; False emits a body
    fragment that links ``{asset_prefix}techtree.css`` / ``techtree.js``.
    """
    import jinja2

    env = jinja2.Environment(
        loader=jinja2.FileSystemLoader(os.path.join(HERE, "templates")),
        autoescape=jinja2.select_autoescape(["html"]),
        keep_trailing_newline=True,
    )
    labels = dict(UI_DEFAULT)
    labels.update(ui or {})
    data = page_data(graph, hrefs, lang_labels, cards)
    blob = json.dumps(data, sort_keys=True, ensure_ascii=True, separators=(",", ":")).replace(
        "</", "<\\/"
    )
    return env.get_template("techtree.html").render(
        title=title or ("Tech tree: " + faction),
        faction=faction,
        svg=render_svg(graph, lay, icon_map, lang_labels),
        data_json=blob,
        ui=labels,
        ui_json=json.dumps(labels, sort_keys=True, ensure_ascii=True).replace("</", "<\\/"),
        standalone=standalone,
        css=_read("static/techtree.css") if standalone else "",
        js=_read("static/techtree.js") if standalone else "",
        asset_prefix=asset_prefix,
        stats=lay["stats"],
        counts={"nodes": len(graph["nodes"]), "edges": len(graph["edges"]), "ranks": lay["ranks"]},
    )


# ---------------------------------------------------------------- checks
def check_graph(graph, lay=None, svg=None, expect_ids=None):
    """Return a list of problems (empty = OK)."""
    bad = []
    nodes = graph["nodes"]
    for a, b in graph["edges"]:
        if a not in nodes or b not in nodes:
            bad.append(f"dangling edge {a} -> {b}")
    cyc = find_cycles(graph) if not bad else []
    if cyc:
        bad.append(f"cycle among {len(cyc)} nodes: {', '.join(cyc[:8])}")
    if expect_ids is not None:
        miss = sorted(set(expect_ids) - set(nodes))
        if miss:
            bad.append("inventions missing from graph: {}".format(", ".join(miss[:8])))
    if lay is not None:
        miss = sorted(set(nodes) - set(lay["pos"]))
        if miss:
            bad.append("nodes without position: {}".format(", ".join(miss[:8])))
        hidden = set(lay["hidden_edges"])
        for a, b in graph["edges"]:
            if (
                (a, b) not in lay["edges"]
                and (a, b) not in lay["back_edges"]
                and (a, b) not in hidden
            ):
                bad.append(f"edge without path {a} -> {b}")
    if svg is not None:
        for i in nodes:
            n = svg.count("<g class=\"tt-node ") and svg.count(f"data-id={quoteattr(i)} ")
            if n != 1:
                bad.append(f"node {i} appears {n} times in svg")
    return bad


def build_all(model, icon_map=None, lang_labels=None):
    """(graph, {faction: (sub_graph, layout, svg)}) for alien and human."""
    g = build_graph(model, lang_labels)
    out = {}
    for f in ("alien", "human"):
        sub = faction_graph(g, f)
        lay = layout(sub)
        out[f] = (sub, lay, render_svg(sub, lay, icon_map, lang_labels))
    return g, out


def _synthetic():
    from ..cfgkit.parse import parse_text

    def rec(name, typ, *deps):
        body = "".join(f' DEPEND "{d}"' for d in deps)
        return f'PROGRESS "{name}"{body} TYPE "{typ}" END'

    objs = """
DEFINE "R_aBAZA" 14
DEFINE "R_aCHATKA" 18
DEFINE "R_aFABRYKA" 1
DEFINE "R_aPORT" 12
DEFINE "R_aZALOGA1" 1
DEFINE "R_aZALOGA2" 2
BUILDING "N_alien_Port" INVENTION "V_alien_port" TYPE "R_aPORT" EQUIVALENT "N_alien_Rocket" END
UNIT "N_alien_Rocket" INVENTION "V_alien_rocket" EQUIVALENT "N_alien_Port" END
UNIT "N_alien_Sq1" INVENTION "V_alien_sq1" TYPE "R_aZALOGA1" END
UNIT "N_alien_Sq2" INVENTION "V_alien_sq2" TYPE "R_aZALOGA2" END
BUILDING "N_alien_Baza" INVENTION "V_alien_base" TYPE "R_aBAZA" END
BUILDING "N_alien_Hut" INVENTION "V_alien_hut" TYPE "R_aCHATKA" END
BUILDING "N_alien_Fab" INVENTION "V_alien_fab" TYPE "R_aFABRYKA" END
PLANET "N_p1" INVENTION "V_planeta_1" END
PLANET "N_p2" INVENTION "V_planeta_2" END
PLANET "N_p3" INVENTION "V_planeta_3" END
SYSTEM "N_s1" INVENTION "V_uklad1" PLANET 1 "N_p1" PLANET 2 "N_p2" END
SYSTEM "N_s2" INVENTION "V_uklad2" PLANET 1 "N_p3" END"""
    text = (
        objs
        + "\n"
        + "\n".join(
            [
                rec("V_planeta_1", "planeta"),
                rec("V_planeta_2", "planeta", "V_planeta_1"),
                rec("V_planeta_3", "planeta"),
                rec("V_uklad1", "uklad"),
                rec("V_uklad2", "uklad", "V_uklad1"),
                rec("V_alien_base", "budynek", "V_alien_A"),
                rec("V_alien_hut", "budynek", "V_alien_A"),
                rec("V_alien_fab", "budynek", "V_alien_A"),
                rec("V_alien_A", "budynek", "V_alien_A"),
                rec("V_alien_B", "pojazd", "V_alien_A"),
                rec("V_alien_C", "pojazd", "V_alien_A"),
                rec("V_alien_D", "projekt", "V_alien_B", "V_alien_C", "V_planeta_1"),
                rec("V_alien_E", "upgrade", "V_alien_A", "V_alien_D"),
                rec("V_alien_port", "budynek", "V_alien_A"),
                rec("V_alien_rocket", "pojazd", "V_alien_A"),
                rec("V_alien_sq1", "pojazd", "V_alien_A"),
                rec("V_alien_sq2", "pojazd", "V_alien_sq1"),
                rec("V_human_A", "budynek"),
            ]
        )
    )
    m = parse_text(text)
    if len(m.classes["PROGRESS"]) != 18:
        raise AssertionError(f"synthetic cfg did not parse: {m.issues[:3]}")
    return m


def selftest(model=None):
    """Synthetic (always) + optional real-model checks; raises AssertionError on failure."""
    for m in (_synthetic(), model):
        if m is None:
            continue
        every = [r["__name__"] for r in m.classes["PROGRESS"]]
        runs = []
        for _ in range(2):
            g, per = build_all(m)
            runs.append("".join(per[f][2] for f in sorted(per)))
        assert runs[0] == runs[1], "SVG not deterministic"
        seen = set()
        for f in sorted(per):
            sub, lay, svg = per[f]
            bad = check_graph(sub, lay, svg)
            assert not bad, bad
            seen |= {i for i, n in sub["nodes"].items() if n["faction"] == f}
        shared = {i for i, n in g["nodes"].items() if n["faction"] == "shared"}
        assert seen | shared | set(g["dropped"]) == set(every), "inventions lost"
        assert not (seen & shared)
        assert not check_graph(g, expect_ids=[i for i in every if i not in g["dropped"]])
    g, per = build_all(_synthetic())
    sub, lay, _ = per["alien"]
    assert "V_planeta_1" in sub["nodes"] and "V_planeta_1" in per["human"][0]["nodes"]  # gates
    # main base and civil buildings are not in the tree (and are reported as dropped)
    assert "V_alien_base" not in sub["nodes"] and "V_alien_hut" not in sub["nodes"]
    assert set(g["dropped"]) == {"V_alien_base", "V_alien_hut", "V_alien_sq2"}, g["dropped"]
    assert "V_alien_sq1" in sub["nodes"]  # a single soldier stays; the squad of 2 is merged
    assert "V_alien_fab" in sub["nodes"]
    # game rules the cfg lacks: system <- all its planets, entry planet of system 2 <- system 1
    nd = g["nodes"]
    assert nd["V_uklad1"]["deps"] == ["V_planeta_1", "V_planeta_2"], nd["V_uklad1"]["deps"]
    assert nd["V_planeta_3"]["deps"] == ["V_uklad1"]
    assert nd["V_uklad2"]["deps"] == ["V_planeta_3", "V_uklad1"], nd["V_uklad2"]["deps"]
    assert ("V_planeta_3", "V_uklad2") in g["derived"]
    assert ("V_uklad1", "V_planeta_3") in g["derived"]
    assert ("V_uklad1", "V_uklad2") not in g["derived"]  # that edge is in the config
    assert ("V_planeta_2", "V_uklad1") in sub["derived"]
    assert "tt-edge gate derived" in per["alien"][2]
    # the rocket is spawned by the spaceport: derived edge on top of its (kept) cfg edge
    assert nd["V_alien_rocket"]["deps"] == ["V_alien_A", "V_alien_port"], nd["V_alien_rocket"]
    assert ("V_alien_port", "V_alien_rocket") in g["derived"]
    assert lay["pos"]["V_alien_rocket"][2] > lay["pos"]["V_alien_port"][2]
    assert not find_cycles(g)
    assert lay["pos"]["V_alien_E"][2] == 3, lay["pos"]["V_alien_E"]
    assert g["nodes"]["V_alien_A"]["start"], g["issues"]
    assert any(m.startswith("self-dependency") for m in g["issues"])
    return True


# ---------------------------------------------------------------- CLI
def _main(argv=None):
    import argparse
    import time

    ap = argparse.ArgumentParser(description="Build tech-tree preview pages")
    ap.add_argument("--game", help="retail dir or .zip (fnt.Source)")
    ap.add_argument(
        "--cfg-text", help="pre-extracted INIT+INITLANG text (utf-8), skips pack reading"
    )
    ap.add_argument("--out", default="tmp/wiki_tree")
    a = ap.parse_args(argv)
    t0 = time.time()
    selftest()
    print("synthetic selftest OK")
    if a.cfg_text:
        from ..cfgkit.parse import parse_text

        with open(a.cfg_text, encoding="utf-8") as fh:
            model = parse_text(fh.read())
    elif a.game:
        model = load_model(a.game)
    else:
        return 0
    t1 = time.time()
    selftest(model)
    g, per = build_all(model)
    os.makedirs(a.out, exist_ok=True)
    print(f"model {t1 - t0:.1f}s, checks+build+layout+svg {time.time() - t1:.2f}s")
    for f in sorted(per):
        sub, lay, _svg = per[f]
        own = sum(1 for n in sub["nodes"].values() if n["faction"] == f)
        st = lay["stats"]
        print(
            f"{f}: nodes {len(sub['nodes'])} (own {own}, shared gates {len(sub['nodes']) - own})"
            f" edges {len(sub['edges'])} (drawn {st['edges_drawn']}) ranks {lay['ranks']}"
            f" crossings {st['crossings_before']} -> {st['crossings_after']}"
            f" dummies {st['dummies']} back-edges {st['back_edges']}"
        )
        html = render_page(sub, lay, f, hrefs=default_hrefs(sub, ""), title="Tech tree - " + f)
        path = os.path.join(a.out, f"techtree_{f}.html")
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(html)
        print("  wrote", path, f"({len(html)} bytes)")
    for msg in g["issues"]:
        print("issue:", msg)
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
