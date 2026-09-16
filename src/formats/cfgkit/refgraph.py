"""Cross-reference graph over a parsed cfg :class:`~cfgkit.parse.Model`.

Every object field that names another entity (``grammar.REF`` plus the two the
REF table misses — ``ANIM.sprite`` and resource ore names) is an edge from the
referencing object to the named resource. Inverting those edges gives, for each
resource, the exact set of objects that use it — the **fan-in** that decides
whether a resource is *exclusively owned* by one object (fold it into that
object's file) or *shared* by several (keep it in a shared file).

This is the measurement engine behind the deferred per-object YAML layout
(``cfg_decompile --layout per-object``): the layout generator asks
:func:`ownership` "who owns each anim/icon?"; the :func:`main` report is the
evidence for the co-location *rule* itself (how clean is "fan-in 1 → own"?).

Read-only; pure function of the Model (no game files, no Ghidra).
"""
from __future__ import annotations

import argparse
import sys
from collections import defaultdict, namedtuple

from . import grammar as G
from ._paths import INIT_DEFAULT, LANG_DEFAULT
from .aliases import CLASS_NS, model_names
from .emit import _reverse_map

# A directed edge: object (src_cls, src_name) --field--> resource (ns, name).
Edge = namedtuple("Edge", "src_cls src_name field universe ns name")

# Union reference universes resolve to one of several namespaces by membership.
_UNION = {
    "art": ("sprites", "anims"),
    "invention": ("defines", "inventions"),
    "unit_or_bldg": ("units", "buildings"),
    "obj": tuple(CLASS_NS.values()),
    # singletons (name matches its namespace)
    "sprite": ("sprites",), "icon": ("icons",), "define": ("defines",),
    "weapon": ("weapons",), "building": ("buildings",), "planet": ("planets",),
    "progress": ("inventions",), "resources": ("resources",),
}


def _ref_targets(kind, val):
    """The name token(s) a single field value points at (mirrors aliases._xlate_field)."""
    if kind == G.STR:
        return [val]
    if kind == G.SLOT_STR:
        return list(val.values())
    if kind == G.CAP:
        return [n for n, _ in val.values()]
    if kind == G.DEPEND:
        return list(val)
    if kind == G.RES:
        return [n for n, _ in val]
    return []


def _iter_ref_fields(cls):
    """(field, kind, universe) for every name-bearing field of a class:
    grammar.REF cross-refs plus RES ore names (whose universe REF omits)."""
    rev = _reverse_map(cls)
    for field, (kind, _kw) in rev.items():
        universe = G.REF.get((cls, field))
        if universe is None and kind == G.RES:
            universe = "resources"
        if universe is not None:
            yield field, kind, universe


def _resolve_ns(universe, name, present):
    """Which namespace a ref target actually lives in (resolve union universes by
    membership). Returns the namespace name, or None if the target is dangling."""
    for ns in _UNION[universe]:
        if name in present[ns]:
            return ns
    return None


def build(model):
    """Return ``(edges, by_target)`` where *edges* is a list of :class:`Edge` and
    *by_target* maps ``(ns, name)`` -> list of the objects that reference it."""
    present = model_names(model)
    edges = []
    for cls, recs in model.classes.items():
        rev = _reverse_map(cls)
        fields = list(_iter_ref_fields(cls))
        for rec in recs:
            src = rec["__name__"]
            for field, kind, universe in fields:
                if field not in rec:
                    continue
                for tgt in _ref_targets(kind, rec[field]):
                    if not tgt:
                        continue  # unset optional (ObjectInit default "") — not a ref
                    ns = _resolve_ns(universe, tgt, present)
                    edges.append(Edge(cls, src, field, universe, ns, tgt))
    by_target = defaultdict(list)
    for e in edges:
        by_target[(e.ns, e.name)].append(e)
    return edges, by_target


# --- ownership: the per-object-layout decision ------------------------------

Owner = namedtuple("Owner", "ns name fanin owners referrers")


def ownership(model, kinds=("anims", "icons")):
    """For each resource of the given foldable *kinds*, the distinct owning
    objects. ``fanin`` counts distinct owner objects (an object using a resource
    in two fields still owns it once). Resources with no cfg referrer have
    ``fanin == 0`` (referenced only by code, or dead)."""
    present = model_names(model)
    _edges, by_target = build(model)
    out = []
    for ns in kinds:
        for name in sorted(present[ns]):
            refs = by_target.get((ns, name), [])
            owners = sorted({(e.src_cls, e.src_name) for e in refs})
            out.append(Owner(ns, name, len(owners), owners, refs))
    return out


def _protected(name):
    return (name in G.PROTECTED_NAMES
            or any(name.startswith(p) for p in G.PROTECTED_PREFIXES))


def main(argv=None):
    from .parse import parse_files
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--init", default=INIT_DEFAULT)
    ap.add_argument("--lang", default=LANG_DEFAULT)
    ap.add_argument("--show", type=int, default=12, help="how many examples per bucket")
    args = ap.parse_args(argv)

    model = parse_files(args.init, args.lang)
    edges, by_target = build(model)
    present = model_names(model)

    # art-field resolution: how many art refs land on sprites (top-level, not
    # foldable) vs anims (foldable)?
    art = [e for e in edges if e.universe == "art"]
    art_ns = defaultdict(int)
    for e in art:
        art_ns[e.ns or "DANGLING"] += 1
    print("=== art-field ref resolution (sprite = stays top-level, anim = foldable) ===")
    for ns, n in sorted(art_ns.items()):
        print(f"  {ns or 'dangling':10} {n:>5} art refs")

    # foldability by kind
    for kind in ("anims", "icons"):
        own = [o for o in ownership(model, (kind,))]
        buckets = defaultdict(list)
        for o in own:
            b = 0 if o.fanin == 0 else 1 if o.fanin == 1 else 2 if o.fanin == 2 else 3
            buckets[b].append(o)
        total = len(own)
        prot = [o for o in own if _protected(o.name)]
        print(f"\n=== {kind}: fan-in distribution ({total} total) ===")
        print(f"  orphan  (0 refs)        : {len(buckets[0]):>4}  (code-only or dead)")
        print(f"  EXCLUSIVE (1 owner)     : {len(buckets[1]):>4}  <- fold into that object's file")
        print(f"  shared    (2 owners)    : {len(buckets[2]):>4}")
        print(f"  shared    (3+ owners)   : {len(buckets[3]):>4}")
        maxo = max(own, key=lambda o: o.fanin, default=None)
        if maxo:
            print(f"  max fan-in              : {maxo.fanin}  ({maxo.name})")
        print(f"  protected names present : {len(prot)}  "
              f"(must stay shared regardless of fan-in)")

        # protected-name fan-in reality check
        pex = [o for o in prot if o.fanin <= 1]
        if pex:
            print(f"  -> {len(pex)} protected name(s) have fan-in <=1 but must NOT be folded:")
            for o in pex[:args.show]:
                print(f"       {o.name}  (fanin {o.fanin})")

        # cross-class shared examples (the messy middle)
        multi = [o for o in own if len({c for c, _ in o.owners}) > 1]
        if multi:
            print(f"  cross-CLASS shared (owned by >1 class): {len(multi)}")
            for o in sorted(multi, key=lambda o: -o.fanin)[:args.show]:
                cls = sorted({c for c, _ in o.owners})
                print(f"       {o.name:22} fanin {o.fanin:>2}  classes {cls}")

        # a few exclusive examples
        print(f"  exclusive examples:")
        for o in buckets[1][:args.show]:
            (c, n), = o.owners
            print(f"       {o.name:22} <- {c} {n}")

    print("\n(read the buckets above to judge the co-location ownership rule)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
