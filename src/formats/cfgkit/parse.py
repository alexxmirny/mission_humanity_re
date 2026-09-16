"""Canonical parser: retail cfg text -> typed :class:`Model`.

Single forward pass that mirrors the game's pass-2 (``ReadObjects`` /
``HandleConfigEntry``) open/commit lifecycle, but into dynamic Python lists (no
pre-count needed — the census is derived from the model afterwards). Unlike the
game, which silently skips tokens it doesn't recognise, this parser records
every unrecognised dispatch-position token as an *issue* — that's the safety
net cfgkit exists to add. Comment and block-comment handling (§1) is applied at
dispatch position only.
"""
from __future__ import annotations

from collections import namedtuple

from . import grammar as G
from . import textio as T

Issue = namedtuple("Issue", "source line message")


class Model:
    """The parsed config, mirroring the game's runtime ``dynamic::data``."""

    def __init__(self):
        self.defines = []      # [(name, int)]
        self.def_banks = []    # [(name, int)]  (DEF_BANK — bank base ids)
        self.icons = []        # [(name, int)]
        self.texts = []        # [(key, wchar_str)]
        self.banks = []        # [{"name": str, "sprites": [(name, frames)]}]
        self.globals = {}      # modifier keyword -> value
        self.classes = {c: [] for c in G.CLASS_GRAMMAR if c != "BANK"}
        self.issues = []       # [Issue]

    def census(self) -> dict:
        c = {
            "DEFINE": len(self.defines), "DEF_BANK": len(self.def_banks),
            "ICON": len(self.icons), "TEXT": len(self.texts),
            "BANK_section": len(self.banks),
            "SPRITE": sum(len(b["sprites"]) for b in self.banks),
        }
        for cls, recs in self.classes.items():
            c[cls] = len(recs)
        c["ANIM_length_total"] = sum(r["length"] for r in self.classes["ANIM"])
        return c


def parse_files(init_path: str, lang_path: str | None = None) -> Model:
    """Parse INIT.CFG and (optionally) INITLANG.CFG into one :class:`Model`."""
    model = Model()
    _feed(model, T.load(init_path), _basename(init_path))
    if lang_path:
        _feed(model, T.load(lang_path), _basename(lang_path))
    return model


def _basename(p):
    import os
    return os.path.basename(p)


def parse_text(text: str, source: str = "<text>") -> Model:
    model = Model()
    _feed(model, text, source)
    return model


def _feed(model, text, source):
    section = None        # current class name, or None at top level
    rec = None            # current staging record
    bank = None           # current BANK dict when section == "BANK"
    in_block = False      # inside /* */

    for lineno, line in enumerate(T.iter_lines(text), 1):
        toks = T.tokenize(line)
        i, n = 0, len(toks)

        def issue(msg, _l=lineno):
            model.issues.append(Issue(source, _l, msg))

        while i < n:
            tk = toks[i]

            if in_block:
                if T.is_block_close(tk):
                    in_block = False
                i += 1
                continue
            if T.is_block_open(tk):
                in_block = True
                i += 1
                continue
            if T.is_line_comment(tk):
                break  # rest of the line is a comment
            if tk.is_str:
                issue(f"unexpected quoted string at dispatch position: {tk.value!r}")
                i += 1
                continue
            if not tk.value.strip():
                # whitespace-only bare token (e.g. a stray trailing tab): the game
                # matches no keyword and silently skips it — so do we, no issue.
                i += 1
                continue

            kw = tk.value

            # ---------------- inside a section ----------------
            if section is not None:
                if kw == "END":
                    if section == "BANK":
                        bank = None
                    else:
                        model.classes[section].append(rec)
                    section, rec = None, None
                    i += 1
                    continue
                spec = G.CLASS_GRAMMAR[section].get(kw)
                if spec is None:
                    issue(f"unrecognized keyword in {section}: {kw!r}")
                    i += 1
                    continue
                i = _apply(spec, toks, i, rec, bank, issue)
                continue

            # ---------------- top level ----------------
            if kw == "END":
                i += 1  # stray END outside a section — game ignores it
                continue
            if kw == "DEFINE":
                i = _flat(model.defines, toks, i, G.INT, issue, "DEFINE")
                continue
            if kw == "DEF_BANK":
                i = _flat(model.def_banks, toks, i, G.INT, issue, "DEF_BANK")
                continue
            if kw == "ICON":
                i = _flat(model.icons, toks, i, G.INT, issue, "ICON")
                continue
            if kw == "TEXT":
                i = _text(model.texts, toks, i, issue)
                continue
            if kw in G.MODIFIERS:
                i = _modifier(model, kw, toks, i, issue)
                continue
            if kw in G.SECTION_OPENERS:
                name_tok = toks[i + 1] if i + 1 < n else None
                if name_tok is None or not name_tok.is_str:
                    issue(f"section opener {kw!r} not followed by a quoted name")
                    i += 1
                    continue
                section = kw
                if kw == "BANK":
                    bank = {"name": name_tok.value, "sprites": []}
                    model.banks.append(bank)
                    rec = None
                else:
                    rec = G.new_record(kw)
                    rec["__name__"] = name_tok.value  # object identity (emitted on the opener)
                i += 2
                continue

            issue(f"unrecognized top-level token: {kw!r}")
            i += 1

    if section is not None:
        model.issues.append(Issue(source, lineno,
                                  f"section {section!r} left open at EOF (object lost)"))
    return model


# ---------------------------------------------------------------- helpers

def _int(tok, issue, what):
    v, ok = T.scan_int(tok.value) if tok and not tok.is_str else (0, False)
    if not ok:
        issue(f"{what}: expected int, got {tok!r}")
    return v


def _f32(tok, issue, what):
    v, ok = T.scan_float(tok.value) if tok and not tok.is_str else (0.0, False)
    if not ok:
        issue(f"{what}: expected float, got {tok!r}")
    return G.f32(v)


def _str(tok, issue, what):
    if tok is None or not tok.is_str:
        issue(f"{what}: expected quoted string, got {tok!r}")
        return ""
    return tok.value


def _get(toks, i):
    return toks[i] if 0 <= i < len(toks) else None


def _flat(dst, toks, i, kind, issue, what):
    name = _str(_get(toks, i + 1), issue, what)
    val = _int(_get(toks, i + 2), issue, what)
    dst.append((name, val))
    return i + 3


def _text(dst, toks, i, issue):
    key = _str(_get(toks, i + 1), issue, "TEXT")
    val = _str(_get(toks, i + 2), issue, "TEXT")  # value stays WCHAR (no narrowing)
    dst.append((key, val))
    return i + 3


def _modifier(model, kw, toks, i, issue):
    tok = _get(toks, i + 1)
    val = _f32(tok, issue, kw) if G.MODIFIERS[kw] == G.F32 else _int(tok, issue, kw)
    model.globals[kw] = val
    return i + 2


def _apply(spec, toks, i, rec, bank, issue):
    kind, field = spec
    g = lambda k: _get(toks, i + k)

    if kind == G.INT:
        rec[field] = _int(g(1), issue, field)
        return i + 2
    if kind == G.F32:
        rec[field] = _f32(g(1), issue, field)
        return i + 2
    if kind == G.STR:
        rec[field] = _str(g(1), issue, field)
        return i + 2
    if kind == G.FLAG:
        rec[field] = 1
        return i + 1
    if kind == G.SLOT_STR:
        slot = _int(g(1), issue, field)
        rec[field][slot] = _str(g(2), issue, field)
        return i + 3
    if kind == G.SLOT_INT:
        slot = _int(g(1), issue, field)
        rec[field][slot] = _int(g(2), issue, field)
        return i + 3
    if kind == G.SLOT_F32:
        slot = _int(g(1), issue, field)
        rec[field][slot] = _f32(g(2), issue, field)
        return i + 3
    if kind == G.CAP:
        slot = _int(g(1), issue, field)
        name = _str(g(2), issue, field)
        val = _f32(g(3), issue, field)
        rec[field][slot] = (name, val)
        return i + 4
    if kind == G.RES:
        val = _int(g(1), issue, field)          # value first
        name = _str(g(2), issue, field)
        rec[field].append((name, val))
        return i + 3
    if kind == G.DEPEND:
        rec[field].append(_str(g(1), issue, field))
        return i + 2
    if kind == G.AREA:
        col = _int(g(1), issue, field)
        col = max(0, min(9, col))               # game clamps 0..9
        j, row = i + 2, 0
        while j < len(toks) and row < 10:
            t = toks[j]
            if t.is_str:
                break
            v, ok = T.scan_int(t.value)
            if not ok:
                break
            rec[field][row][col] = v
            row += 1
            j += 1
        return j
    if kind == G.BANK_RANGE:
        lo = _int(g(1), issue, field)
        hi = _int(g(2), issue, field)
        rec[field].update(range(lo, hi + 1))    # inclusive
        return i + 3
    if kind == G.SOURCE4:
        rec[field] = [_int(g(k), issue, field) for k in (1, 2, 3, 4)]
        return i + 5
    if kind == G.BANK_SPRITE:
        name = _str(g(1), issue, "SPRITE")
        frames = _int(g(2), issue, "SPRITE")
        bank["sprites"].append((name, frames))
        return i + 3

    issue(f"internal: unknown arg kind {kind!r}")
    return i + 1
