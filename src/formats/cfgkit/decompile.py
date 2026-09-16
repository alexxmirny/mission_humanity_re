"""Schema-light decompiler/compiler: Model <-> plain (YAML/JSON-safe) dict.

This is the Phase-2 "dict mode": a lossless, serialization-friendly projection
of the typed model with ObjectInit defaults omitted (``decompile``) and
re-applied on the way back (``compile_``). No pydantic schema, no English
remodelling, no alias table yet — those are Phase 3/4. Field keys are the
model's field names; slotted/compound arrays become JSON-safe lists.

Round-trip contract (the Phase-2 gate): ``compile_(decompile(m))`` is canon-equal
to ``m`` for retail, and ``decompile(m)`` survives ``json.dumps``.
"""
from __future__ import annotations

from . import grammar as G
from .emit import _reverse_map
from .parse import Model


def _field_out(kind, val):
    if kind in (G.INT, G.F32, G.STR, G.FLAG):
        return val
    if kind in (G.SLOT_STR, G.SLOT_INT, G.SLOT_F32):
        return [[s, val[s]] for s in sorted(val)]
    if kind == G.CAP:
        return [[s, val[s][0], val[s][1]] for s in sorted(val)]
    if kind == G.RES:
        return [[name, v] for name, v in val]
    if kind == G.DEPEND:
        return list(val)
    if kind == G.AREA:
        out = []
        for col in range(10):
            column = [val[row][col] for row in range(10)]
            last = max((r for r in range(10) if column[r] != 0), default=-1)
            if last >= 0:
                out.append([col, column[:last + 1]])
        return out
    if kind == G.BANK_RANGE:
        from .emit import _ranges
        return [[lo, hi] for lo, hi in _ranges(val)]
    if kind == G.SOURCE4:
        return list(val)
    raise ValueError(f"decompile: unknown kind {kind!r}")


def _field_in(kind, data, dst):
    # F32 fields are re-quantized through binary32 here — the game parses cfg
    # text the same way, so a hand-typed "0.1" becomes the exact game value and
    # already-quantized values are unchanged (idempotent).
    if kind == G.F32:
        return G.f32(data)
    if kind in (G.INT, G.STR, G.FLAG):
        return data
    if kind == G.SLOT_F32:
        return {int(s): G.f32(v) for s, v in data}
    if kind in (G.SLOT_STR, G.SLOT_INT):
        return {int(s): v for s, v in data}
    if kind == G.CAP:
        return {int(s): (name, G.f32(v)) for s, name, v in data}
    if kind == G.RES:
        return [(name, v) for name, v in data]
    if kind == G.DEPEND:
        return list(data)
    if kind == G.AREA:
        grid = [[0] * 10 for _ in range(10)]
        for col, rows in data:
            for row, v in enumerate(rows):
                grid[row][col] = v
        return grid
    if kind == G.BANK_RANGE:
        s = set()
        for lo, hi in data:
            s.update(range(lo, hi + 1))
        return s
    if kind == G.SOURCE4:
        return list(data)
    raise ValueError(f"compile: unknown kind {kind!r}")


def decompile(model) -> dict:
    classes = {}
    for cls, recs in model.classes.items():
        rev = _reverse_map(cls)
        out = []
        for rec in recs:
            default = G.new_record(cls)
            obj = {"name": rec["__name__"]}
            for field, (kind, _kw) in rev.items():
                val = rec.get(field)
                if val == default.get(field):
                    continue                       # omit defaults
                obj[field] = _field_out(kind, val)
            out.append(obj)
        classes[cls] = out
    return {
        "defines": [[n, v] for n, v in model.defines],
        "def_banks": [[n, v] for n, v in model.def_banks],
        "icons": [[n, v] for n, v in model.icons],
        "texts": [[k, v] for k, v in model.texts],
        "banks": [{"name": b["name"], "sprites": [[s, f] for s, f in b["sprites"]]}
                  for b in model.banks],
        "globals": dict(model.globals),
        "classes": classes,
    }


def compile_(data: dict) -> Model:
    model = Model()
    model.defines = [(n, v) for n, v in data["defines"]]
    model.def_banks = [(n, v) for n, v in data["def_banks"]]
    model.icons = [(n, v) for n, v in data["icons"]]
    model.texts = [(k, v) for k, v in data["texts"]]
    model.banks = [{"name": b["name"], "sprites": [(s, f) for s, f in b["sprites"]]}
                   for b in data["banks"]]
    model.globals = dict(data["globals"])
    for cls, objs in data["classes"].items():
        rev = _reverse_map(cls)
        for obj in objs:
            rec = G.new_record(cls)
            rec["__name__"] = obj["name"]
            for field, raw in obj.items():
                if field == "name":
                    continue
                kind = rev[field][0]
                rec[field] = _field_in(kind, raw, rec)
            model.classes[cls].append(rec)
    return model
