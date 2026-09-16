"""Emitter: typed :class:`~cfgkit.parse.Model` -> game-ready cfg text.

Produces a UTF-16 LE + BOM, CRLF file that re-parses to a canon-equal model
(the cfg grammar notes §7 rules). Design choices:

- **Default omission**: a field equal to its ObjectInit default is not emitted
  (the game re-applies the default at ``open``), so output stays retail-lean and
  never emits empty ``INVENTION ""``-style lines.
- **Floats**: :func:`fmt_float` emits the shortest decimal that round-trips
  through binary32 back to the stored value (``double(float32(text))``).
- **Flattened section order** (DEFINE, DEF_BANK, BANK, ICON, then classes in
  retail block order): the whole point of cfgkit is to re-group config by
  meaning, so the emitter must not depend on retail's cross-class interleaving.
  Whether the game accepts a flattened file is the Phase-2 boot test; if it
  doesn't, an order-manifest mode is the designed fallback. Objects **within** a
  class keep file/index order (index-semantic).
- **Shared arrays**: ``unit_name`` emits as ``PRODUCTION``, ``unit_quant`` as
  ``PRODUCTION_TIME``, ``extract_fuel`` as ``EXTRACT`` — the game stores these in
  one array regardless of the PRODUCTION/TRANSPORT resp. EXTRACT/FUEL keyword, so
  the runtime record is identical.
- **PROBABILYTY** keyword is emitted with the binary's misspelling; tabs are
  never emitted; names/strings are always quoted.

:func:`emit` returns ``(init_text, lang_text)``; :func:`write_files` encodes both
to disk.
"""
from __future__ import annotations

import os

from . import grammar as G

# Class emission order (retail top-level block order; PROJECT/UPGRADE flattened).
CLASS_ORDER = ["TREE", "ANIM", "WEAPON", "UNIT", "BUILDING", "PROJECT",
               "UPGRADE", "SYSTEM", "PLANET", "PROGRESS", "STONE"]

IND = "   "  # indentation for section bodies (spaces only — never a tab)


def fmt_float(v: float) -> str:
    """Shortest decimal text that parses back through binary32 to *v*."""
    if v == int(v) and abs(v) < 1e15:
        return str(int(v))
    for prec in (6, 7, 8, 9, 17):
        s = f"{v:.{prec}g}"
        if G.f32(float(s)) == v:
            return s
    return repr(v)


def _qstr(s: str) -> str:
    if '"' in s:
        raise ValueError(f"cannot emit string containing a quote: {s!r}")
    return f'"{s}"'


def _reverse_map(cls):
    """field -> (kind, keyword); first keyword per field wins (shared arrays)."""
    rev = {}
    for kw, (kind, field) in G.CLASS_GRAMMAR[cls].items():
        rev.setdefault(field, (kind, kw))
    return rev


def _ranges(nums):
    """Compress a set of ints into inclusive [lo, hi] contiguous ranges."""
    out = []
    for n in sorted(nums):
        if out and n == out[-1][1] + 1:
            out[-1][1] = n
        else:
            out.append([n, n])
    return out


def _emit_field(lines, kind, kw, val):
    if kind == G.INT:
        lines.append(f"{IND}{kw} {val}")
    elif kind == G.F32:
        lines.append(f"{IND}{kw} {fmt_float(val)}")
    elif kind == G.STR:
        lines.append(f"{IND}{kw} {_qstr(val)}")
    elif kind == G.FLAG:
        if val:
            lines.append(f"{IND}{kw}")
    elif kind == G.SLOT_STR:
        for slot in sorted(val):
            lines.append(f"{IND}{kw} {slot} {_qstr(val[slot])}")
    elif kind == G.SLOT_INT:
        for slot in sorted(val):
            lines.append(f"{IND}{kw} {slot} {val[slot]}")
    elif kind == G.SLOT_F32:
        for slot in sorted(val):
            lines.append(f"{IND}{kw} {slot} {fmt_float(val[slot])}")
    elif kind == G.CAP:
        for slot in sorted(val):
            name, v = val[slot]
            lines.append(f"{IND}{kw} {slot} {_qstr(name)} {fmt_float(v)}")
    elif kind == G.RES:
        for name, v in val:                       # value first on the wire
            lines.append(f"{IND}{kw} {v} {_qstr(name)}")
    elif kind == G.DEPEND:
        for name in val:
            lines.append(f"{IND}{kw} {_qstr(name)}")
    elif kind == G.AREA:
        for col in range(10):
            column = [val[row][col] for row in range(10)]
            last = max((r for r in range(10) if column[r] != 0), default=-1)
            if last >= 0:
                nums = " ".join(str(column[r]) for r in range(last + 1))
                lines.append(f"{IND}{kw} {col} {nums}")
    elif kind == G.BANK_RANGE:
        for lo, hi in _ranges(val):
            lines.append(f"{IND}{kw} {lo} {hi}")
    elif kind == G.SOURCE4:
        lines.append(f"{IND}{kw} " + " ".join(str(x) for x in val))
    else:
        raise ValueError(f"cannot emit kind {kind!r}")


def _emit_section(lines, cls, rec):
    default = G.new_record(cls)
    rev = _reverse_map(cls)
    for field, (kind, kw) in rev.items():
        val = rec.get(field)
        if val == default.get(field):
            continue                              # omit fields at their default
        _emit_field(lines, kind, kw, val)


def emit(model):
    """Return ``(init_text, lang_text)`` — both without a leading BOM."""
    L = []
    for name, val in model.defines:
        L.append(f"DEFINE {_qstr(name)} {val}")
    for kw, val in model.globals.items():
        L.append(f"{kw} {fmt_float(val) if G.MODIFIERS[kw] == G.F32 else val}")
    for name, val in model.def_banks:
        L.append(f"DEF_BANK {_qstr(name)} {val}")
    for name, val in model.icons:
        L.append(f"ICON {_qstr(name)} {val}")
    for bank in model.banks:
        L.append(f'BANK {_qstr(bank["name"])}')
        for sname, frames in bank["sprites"]:
            L.append(f"{IND}SPRITE {_qstr(sname)} {frames}")
        L.append("END")
    for cls in CLASS_ORDER:
        for rec in model.classes.get(cls, []):
            L.append(f"{cls} {_qstr(rec['__name__'])}")
            _emit_section(L, cls, rec)
            L.append("END")
    L.append("")  # trailing blank line so INIT.CFG ends with CRLF before the seam
    init_text = "\r\n".join(L) + "\r\n"

    # initlang: BOM lands mid-buffer in the game's concatenation, so start it
    # with a blank line after the BOM (the emitter adds the BOM at encode time).
    T = [""]
    for key, val in model.texts:
        T.append(f"TEXT {_qstr(key)} {_qstr(val)}")
    lang_text = "\r\n".join(T) + "\r\n"
    return init_text, lang_text


def _obj_name(rec):
    return rec.get("__name__", "")


def write_files(model, init_path, lang_path):
    init_text, lang_text = emit(model)
    _write_utf16_bom(init_path, init_text)
    _write_utf16_bom(lang_path, lang_text)


def _write_utf16_bom(path, text):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"\xff\xfe")               # UTF-16 LE BOM
        f.write(text.encode("utf-16-le"))
