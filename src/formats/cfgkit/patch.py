"""Transactional CFG patch oracle.

``merge(model, inputs, locale)`` accepts Input(source, path, bytes, mod_index).
The returned model is independent of the input model. ``prepare`` accepts the
same byte inputs as the native API and returns the BOM-inclusive output pair.
"""
from __future__ import annotations

import copy
import math
from dataclasses import dataclass

from . import emit, grammar as G, parse, textio as T


@dataclass(frozen=True)
class Input:
    source: str
    path: str
    bytes: bytes
    mod_index: int


class PatchError(ValueError):
    pass


def _fail(source, line, message):
    raise PatchError(f"{source}:{line}: {message}")


def _decode(item):
    try:
        text = item.bytes.decode("utf-8-sig", "strict")
    except UnicodeError as exc:
        raise PatchError(f"{item.source}: invalid UTF-8: {exc}") from exc
    if "\x00" in text:
        raise PatchError(f"{item.source}: embedded NUL")
    return text


def _lines(item):
    block = False
    for lineno, line in enumerate(T.iter_lines(_decode(item)), 1):
        out = []
        pos = 0
        while pos < len(line):
            if line[pos] == " ":
                pos += 1
                continue
            if line[pos] == '"' and not block:
                end = line.find('"', pos + 1)
                if end < 0:
                    _fail(item.source, lineno, "unterminated quote")
                token = T.Token(True, line[pos + 1:end])
                pos = end + 1
            else:
                end = line.find(" ", pos)
                if end < 0:
                    end = len(line)
                token = T.Token(False, line[pos:end])
                pos = end
            if block:
                if T.is_block_close(token):
                    block = False
                continue
            if T.is_block_open(token):
                block = True
                continue
            if T.is_line_comment(token):
                break
            if not token.is_str and not token.value.isascii():
                _fail(item.source, lineno, "bare tokens must be ASCII")
            out.append(token)
        if out:
            yield lineno, out
    if block:
        _fail(item.source, lineno, "unterminated comment")


def _index(tokens, pos, source, line):
    if pos == len(tokens):
        return None
    if pos + 2 != len(tokens) or tokens[pos].is_str or tokens[pos].value != "INDEX":
        _fail(source, line, "expected optional INDEX n")
    t = tokens[pos + 1]
    v, ok = T.scan_int(t.value) if not t.is_str else (0, False)
    if not ok or not 1 <= v <= 2147483647:
        _fail(source, line, "invalid original index")
    return v


def _select(records, name, index, get_name, source, line):
    if index is not None:
        if index > len(records) or get_name(records[index - 1]) != name:
            _fail(source, line, f"INDEX {index} does not identify {name!r}")
        return index - 1
    matches = [i for i, rec in enumerate(records) if get_name(rec) == name]
    if len(matches) != 1:
        _fail(source, line, f"target {name!r} is {'unknown' if not matches else 'ambiguous; specify INDEX'}")
    return matches[0]


def _number(token, kind, source, line):
    if token.is_str:
        _fail(source, line, "number must not be quoted")
    if kind == G.INT:
        n, ok = T.scan_int(token.value)
        if not ok or not -2147483648 <= n <= 2147483647:
            _fail(source, line, "integer outside int32")
    else:
        n, ok = T.scan_float(token.value)
        try:
            n = G.f32(n)
        except (OverflowError, ValueError):
            ok = False
        if not ok or not math.isfinite(n):
            _fail(source, line, "invalid finite binary32")
    return n


def _apply_line(rec, bank, cls, toks, source, line, mentioned):
    i = 0
    while i < len(toks):
        token = toks[i]
        if token.is_str:
            _fail(source, line, "expected keyword")
        kw = token.value
        if kw == "@clear":
            if i + 1 >= len(toks) or toks[i + 1].is_str:
                _fail(source, line, "@clear requires a keyword")
            target = toks[i + 1].value
            spec = G.CLASS_GRAMMAR[cls].get(target)
            if spec is None or cls == "BANK":
                _fail(source, line, f"cannot clear {target!r}")
            field = spec[1]
            rec[field] = copy.deepcopy(G.new_record(cls)[field])
            mentioned.add(field)
            i += 2
            continue
        spec = G.CLASS_GRAMMAR[cls].get(kw)
        if spec is None:
            _fail(source, line, f"unknown {cls} keyword {kw!r}")
        kind, field = spec
        counts = {G.INT: 1, G.F32: 1, G.STR: 1, G.FLAG: 0,
                  G.SLOT_STR: 2, G.SLOT_INT: 2, G.SLOT_F32: 2,
                  G.CAP: 3, G.RES: 2, G.DEPEND: 1, G.BANK_RANGE: 2,
                  G.SOURCE4: 4, G.BANK_SPRITE: 2}
        if kind == G.AREA:
            end = i + 2
            if end > len(toks):
                _fail(source, line, "AREA requires a column")
            col = _number(toks[i + 1], G.INT, source, line)
            if not 0 <= col < 10:
                _fail(source, line, "AREA column outside 0..9")
            while end < len(toks) and not toks[end].is_str and T.scan_int(toks[end].value)[1]:
                _number(toks[end], G.INT, source, line)
                end += 1
            if end - i - 2 > 10:
                _fail(source, line, "AREA has more than 10 rows")
            for row in range(10):
                rec[field][row][col] = 0
        else:
            end = i + 1 + counts[kind]
            if end > len(toks):
                _fail(source, line, f"missing arguments to {kw}")
            numeric = {G.INT: [(1, G.INT)], G.F32: [(1, G.F32)],
                G.SLOT_STR: [(1, G.INT)], G.SLOT_INT: [(1, G.INT), (2, G.INT)],
                G.SLOT_F32: [(1, G.INT), (2, G.F32)],
                G.CAP: [(1, G.INT), (3, G.F32)], G.RES: [(1, G.INT)],
                G.BANK_RANGE: [(1, G.INT), (2, G.INT)],
                G.SOURCE4: [(n, G.INT) for n in range(1, 5)],
                G.BANK_SPRITE: [(2, G.INT)]}
            for off, ty in numeric.get(kind, ()):
                _number(toks[i + off], ty, source, line)
            if kind in (G.SLOT_STR, G.SLOT_INT, G.SLOT_F32, G.CAP):
                slot = _number(toks[i + 1], G.INT, source, line)
                mode, cap = G.CAPS[(cls, field)]
                if not (1 if mode == "slot1" else 0) <= slot < cap:
                    _fail(source, line, f"{kw} slot out of range")
            if kind == G.BANK_RANGE:
                lo = _number(toks[i + 1], G.INT, source, line)
                hi = _number(toks[i + 2], G.INT, source, line)
                if not 0 <= lo <= hi < 100:
                    _fail(source, line, "BANK range outside 0..99")
        if kind == G.BANK_SPRITE and field not in mentioned:
            bank["sprites"] = []
            mentioned.add(field)
        if kind in (G.RES, G.DEPEND, G.BANK_RANGE) and field not in mentioned:
            rec[field] = copy.deepcopy(G.new_record(cls)[field])
            mentioned.add(field)
        errors = []
        try:
            consumed = parse._apply(spec, toks[i:end], 0, rec, bank, errors.append)
        except (OverflowError, ValueError, IndexError) as exc:
            _fail(source, line, f"invalid {kw}: {exc}")
        if errors or consumed != end - i:
            _fail(source, line, f"invalid {kw}: {'; '.join(errors)}")
        i = end


def _gameplay(model, item):
    section = None
    for line, toks in _lines(item):
        if any(t.is_str and not t.value.isascii() for t in toks):
            _fail(item.source, line, "modified narrow strings must be ASCII")
        pos = 0
        while pos < len(toks):
            if section is None:
                if (pos + 3 > len(toks) or toks[pos].is_str or
                        toks[pos].value not in ("@patch", "@replace") or
                        toks[pos + 1].is_str or not toks[pos + 2].is_str):
                    _fail(item.source, line, "expected @patch or @replace CLASS \"name\"")
                op, cls, name = toks[pos].value, toks[pos + 1].value, toks[pos + 2].value
                if cls not in G.CLASS_GRAMMAR:
                    _fail(item.source, line, f"unknown class {cls!r}")
                after = pos + 3
                index = None
                if after < len(toks) and not toks[after].is_str and toks[after].value == "INDEX":
                    index = _index(toks[after:after + 2], 0, item.source, line)
                    after += 2
                records = model.banks if cls == "BANK" else model.classes[cls]
                key = (lambda r: r["name"]) if cls == "BANK" else (lambda r: r["__name__"])
                selected = _select(records, name, index, key, item.source, line)
                original = records[selected]
                if cls == "BANK":
                    rec = {"name": name, "sprites": []} if op == "@replace" else copy.deepcopy(original)
                    bank = rec
                else:
                    rec = G.new_record(cls) if op == "@replace" else copy.deepcopy(original)
                    rec["__name__"] = name
                    bank = None
                section = (cls, selected, rec, bank, original, set())
                pos = after
                continue
            cls, selected, rec, bank, original, mentioned = section
            stop = pos
            while stop < len(toks) and (toks[stop].is_str or toks[stop].value != "END"):
                stop += 1
            if stop > pos:
                _apply_line(rec, bank, cls, toks[pos:stop], item.source, line, mentioned)
            if stop < len(toks):
                if cls == "BANK":
                    if rec != original:
                        _fail(item.source, line, "BANK sprite identities/frames cannot change")
                    model.banks[selected] = rec
                else:
                    model.classes[cls][selected] = rec
                section = None
                pos = stop + 1
            else:
                pos = stop
    if section is not None:
        raise PatchError(f"{item.source}: section left open at EOF")


def _texts(model, item):
    for line, toks in _lines(item):
        if len(toks) < 3 or toks[0].is_str or toks[0].value != "TEXT" or not toks[1].is_str or not toks[2].is_str:
            _fail(item.source, line, "expected TEXT \"key\" \"value\" [INDEX n]")
        if not toks[1].value.isascii():
            _fail(item.source, line, "TEXT target key must be ASCII")
        value = toks[2].value
        units = len(value.encode("utf-16-le")) // 2
        if units > G.CAPS[("TEXT", "value")][1]:
            _fail(item.source, line, "TEXT value exceeds UTF-16 staging limit")
        text_id = next((number for name, number in model.defines if name == toks[1].value), None)
        lo, hi = G.DOMAIN[("TEXT", "__resource_label_id__")]
        if text_id is not None and lo <= text_id <= hi:
            if "%" in value.replace("%%", "") or units - value.count("%%") > G.CAPS[("TEXT", "__resource_label__")][1]:
                _fail(item.source, line, "invalid resource-label format or length")
        idx = _index(toks, 3, item.source, line)
        slot = _select(model.texts, toks[1].value, idx, lambda x: x[0], item.source, line)
        model.texts[slot] = (model.texts[slot][0], toks[2].value)


def _path(item):
    p = item.path.replace("/", "\\").lower()
    if p == "cfg\\patch.cfg":
        return "game", ""
    parts = p.split("\\")
    if len(parts) == 2 and parts[0] == "lang" and parts[1].endswith(".cfg") and parts[1][:-4]:
        return "text", parts[1][:-4]
    raise PatchError(f"{item.source}: invalid config input path {item.path!r}")


def _validate_from(model, item):
    try:
        validate_candidate(model)
    except PatchError as exc:
        raise PatchError(f"{item.source}: {exc}") from exc


def merge(base_model, inputs, locale):
    if base_model.issues:
        raise PatchError(str(base_model.issues[0]))
    result = copy.deepcopy(base_model)
    inputs = list(inputs)
    if len(inputs) > 8192 or sum(len(item.bytes) for item in inputs) > 256 * 1024 * 1024:
        raise PatchError("config input set exceeds catalog limits")
    groups = {}
    for item in inputs:
        if len(item.bytes) > 64 * 1024 * 1024:
            raise PatchError(f"{item.source}: input exceeds catalog file limit")
        if not 0 <= item.mod_index <= 0xffffffff:
            raise PatchError(f"{item.source}: mod index outside uint32")
        kind, language = _path(item)
        group = groups.setdefault(item.mod_index, {})
        if (kind, language) in group:
            raise PatchError(f"{item.source}: duplicate config input")
        group[(kind, language)] = item
    ordered = [group for _, group in sorted(groups.items())]
    if not locale and any(kind == "text" for group in ordered for kind, _ in group):
        raise PatchError("locale is required when translation inputs are present")
    for group in ordered:
        game = group.get(("game", ""))
        if game:
            _gameplay(result, game)
    # Resolve and validate every shipped locale after all mods.  English fallback
    # is applied separately for each mod, before that mod's selected language.
    languages = {"en", locale.lower()}
    languages.update(language for group in ordered for kind, language in group if kind == "text")
    chosen = None
    for language in sorted(languages):
        candidate = copy.deepcopy(result)
        context = None
        for group in ordered:
            english = group.get(("text", "en"))
            local = group.get(("text", language)) if language != "en" else None
            if english:
                _texts(candidate, english)
                context = english
            if local:
                _texts(candidate, local)
                context = local
            context = context or group.get(("game", ""))
        if context:
            _validate_from(candidate, context)
        if language == locale.lower():
            chosen = candidate
    return chosen if ordered else result


def prepare(base_init, base_lang, inputs, locale):
    inputs = list(inputs)
    if not inputs:
        return None
    def decode_base(data):
        try:
            text = data.decode("utf-16-le", "strict")
        except UnicodeError as exc:
            raise PatchError(f"invalid base UTF-16: {exc}") from exc
        if text.startswith(T.BOM):
            text = text[1:]
        if "\x00" in text:
            raise PatchError("base contains NUL")
        return text
    model = parse.Model()
    parse._feed(model, decode_base(base_init), "INIT.CFG")
    parse._feed(model, decode_base(base_lang), "INITLANG.CFG")
    if model.issues:
        raise PatchError(str(model.issues[0]))
    result = merge(model, inputs, locale)
    init, lang = emit.emit(result)
    return (b"\xff\xfe" + init.encode("utf-16-le"),
            b"\xff\xfe" + lang.encode("utf-16-le"))


def validate_candidate(model):
    """Validate the complete post-merge model before serialization."""
    def bad(cls, name, field, message):
        raise PatchError(f"{cls} {name!r}.{field}: {message}")

    def string(value, limit, cls, name, field, *, wide=False):
        if not isinstance(value, str) or any(c in value for c in ('\x00', '\r', '\n', '"')):
            bad(cls, name, field, "string cannot be represented")
        try:
            if wide:
                length = len(value.encode("utf-16-le")) // 2
            elif value.isascii():
                length = len(value)
            else:
                bad(cls, name, field, "narrow string must be ASCII")
        except UnicodeError:
            bad(cls, name, field, "invalid Unicode")
        if length > limit:
            bad(cls, name, field, f"string length {length} exceeds {limit}")

    def integer(value, cls, name, field):
        if not isinstance(value, int) or not -2147483648 <= value <= 2147483647:
            bad(cls, name, field, "outside signed int32")

    def real(value, cls, name, field):
        if not isinstance(value, (int, float)) or not math.isfinite(value):
            bad(cls, name, field, "not finite")
        try:
            if not math.isfinite(G.f32(value)):
                bad(cls, name, field, "not finite binary32")
        except (ValueError, OverflowError):
            bad(cls, name, field, "outside binary32")

    for key, value in model.globals.items():
        if key not in G.MODIFIERS:
            bad("GLOBAL", key, "name", "unknown modifier")
        if G.MODIFIERS[key] == G.F32:
            real(value, "GLOBAL", key, "value")
        else:
            integer(value, "GLOBAL", key, "value")
    for cls, field, mul_key, add_key, int_result in G.TRANSFORMS:
        mul = model.globals.get(mul_key, G.MODIFIER_DEFAULTS[mul_key])
        add = model.globals.get(add_key, G.MODIFIER_DEFAULTS[add_key])
        for rec in model.classes[cls]:
            values = [v for _, v in rec[field]] if field == "resources" else [rec[field]]
            for value in values:
                transformed = value * mul + add
                if not math.isfinite(transformed):
                    bad(cls, rec["__name__"], field, "nonfinite modifier result")
                if int_result and not -2147483648 <= math.trunc(transformed) <= 2147483647:
                    bad(cls, rec["__name__"], field, "modified value outside int32")

    definitions = {}
    for label, pairs in (("DEFINE", model.defines), ("DEF_BANK", model.def_banks), ("ICON", model.icons)):
        for name, value in pairs:
            string(name, G.CAPS[("*", "__string__")][1], label, name, "name")
            integer(value, label, name, "value")
            if label == "DEFINE":
                definitions.setdefault(name, value)
    pools = {"sprite": {name for bank in model.banks for name, _ in bank["sprites"]},
             "icon": {name for name, _ in model.icons}, "define": set(definitions),
             "resource_define": set(definitions)}
    for cls, universe in (("ANIM", "anim"), ("UNIT", "unit"),
                          ("BUILDING", "building"), ("WEAPON", "weapon"),
                          ("PLANET", "planet"), ("PROGRESS", "progress")):
        pools[universe] = {rec["__name__"] for rec in model.classes[cls]}
    bank_bases = {}
    sprite_ids = {}
    for bank_name, base in model.def_banks:
        bank_bases.setdefault(bank_name, base)
    for bank in model.banks:
        if bank["name"] not in bank_bases:
            bad("BANK", bank["name"], "name", "missing DEF_BANK")
        cursor = bank_bases[bank["name"]]
        sprite_limit = G.CAPS[("SPRITE", "__id__")][1]
        if not 0 <= cursor <= sprite_limit:
            bad("BANK", bank["name"], "name", "sprite base out of range")
        string(bank["name"], 127, "BANK", bank["name"], "name")
        for name, frames in bank["sprites"]:
            string(name, G.CAPS[("*", "__string__")][1], "BANK", bank["name"], "sprite")
            integer(frames, "BANK", bank["name"], "frames")
            if frames < 0 or cursor + frames > sprite_limit:
                bad("BANK", bank["name"], "frames", "sprite ID range exhausted")
            sprite_ids.setdefault(name, cursor)
            cursor += frames
    for key, value in model.texts:
        string(key, G.CAPS[("*", "__string__")][1], "TEXT", key, "key")
        string(value, G.CAPS[("TEXT", "value")][1], "TEXT", key, "value", wide=True)
        if key not in definitions or not G.DOMAIN[("TEXT", "__id__")][0] <= definitions[key] <= G.DOMAIN[("TEXT", "__id__")][1]:
            bad("TEXT", key, "key", "DEFINE text index outside pointer table")
    if sum(len(s.encode("utf-16-le")) // 2 + 1 for _, s in model.texts) > G.CAPS[("TEXT", "__pool__")][1]:
        bad("TEXT", "*", "value", "text pool exhausted")
    # TEXT is compiled in order; repeated numeric IDs overwrite the pointer.
    text_lengths = {}
    compiled_text = {}
    for key, value in model.texts:
        text_id = definitions[key]
        compiled_text[text_id] = value
        text_lengths[text_id] = len(value.encode("utf-16-le")) // 2
    lo, hi = G.DOMAIN[("TEXT", "__resource_label_id__")]
    rendered_lengths = dict(text_lengths)
    for text_id in range(lo, hi + 1):
        if text_id not in compiled_text:
            bad("TEXT", str(text_id), "value", "missing required resource-label format")
        value = compiled_text[text_id]
        rendered = value.replace("%%", "")
        if "%" in rendered:
            bad("TEXT", str(text_id), "value", "resource label has an unsafe format conversion")
        rendered_units = len(value.encode("utf-16-le")) // 2 - value.count("%%")
        rendered_lengths[text_id] = rendered_units
        if rendered_units > G.CAPS[("TEXT", "__resource_label__")][1]:
            bad("TEXT", str(text_id), "value", "resource label overflows destination")
    header_id = G.DOMAIN[("TEXT", "__info_header_id__")][0]
    label_units = sum(rendered_lengths.get(i, 0) for i in range(lo, hi + 1))
    for cls in ("UNIT", "BUILDING", "PROJECT"):
        for rec in model.classes[cls]:
            costs = [value for _, value in rec["resources"][:4]]
            costs.extend([0] * (4 - len(costs)))
            if cls == "UNIT":
                mul = model.globals.get("UNIT_RESOURCE_MUL", G.MODIFIER_DEFAULTS["UNIT_RESOURCE_MUL"])
                add = model.globals.get("UNIT_RESOURCE_ADD", G.MODIFIER_DEFAULTS["UNIT_RESOURCE_ADD"])
                costs = [math.trunc(value * mul + add) for value in costs]
            length = (text_lengths.get(header_id, 0) + label_units +
                      sum(len(str(value)) for value in costs) + G.CAPS[("TEXT", "__info_fixed__")][1])
            if length > G.CAPS[("TEXT", "__info_scratch__")][1]:
                bad(cls, rec["__name__"], "resources", "info summary text scratch overflow")
    buildings = model.classes["BUILDING"]
    for reason, alien, human in G.HOUSING_REASONS:
        reason_length = text_lengths.get(reason, 0)
        for typ in (alien, human):
            housing = next((r for r in buildings if (definitions.get(r["type"], 0) & 0xff) == typ), None) if typ >= 0 else None
            housing_length = text_lengths.get(definitions.get(housing["__name__"], 0), 0) if housing else 0
            for building in buildings:
                name_length = text_lengths.get(definitions.get(building["__name__"], 0), 0)
                size = name_length + reason_length + housing_length + (6 if housing else 3)
                if size > G.CAPS[("TEXT", "__scratch__")][1]:
                    bad("TEXT", building["__name__"], "value", "production completion text scratch overflow")
    for cls, recs in model.classes.items():
        count_cap = G.CAPS.get((cls, "__count__"))
        if count_cap and len(recs) > count_cap[1]:
            bad(cls, "*", "__count__", "class table exhausted")
        for rec in recs:
            name = rec["__name__"]
            string(name, G.CAPS[("*", "__string__")][1], cls, name, "name")
            seen = set()
            for kind, field in G.CLASS_GRAMMAR[cls].values():
                if field in seen:
                    continue
                seen.add(field)
                value = rec[field]
                limit = G.CAPS.get((cls, field), G.CAPS[("*", "__string__")])[1]
                if kind == G.STR:
                    string(value, limit, cls, name, field)
                elif kind in (G.INT, G.FLAG):
                    integer(value, cls, name, field)
                    if kind == G.FLAG and value not in (0, 1):
                        bad(cls, name, field, "flag must be 0 or 1")
                elif kind == G.F32:
                    real(value, cls, name, field)
                elif kind in (G.SLOT_STR, G.SLOT_INT, G.SLOT_F32, G.CAP):
                    mode, cap = G.CAPS[(cls, field)]
                    for slot, v in value.items():
                        if not isinstance(slot, int) or not (1 if mode == "slot1" else 0) <= slot < cap:
                            bad(cls, name, field, f"invalid slot {slot}")
                        if kind == G.SLOT_STR:
                            string(v, G.CAPS[("*", "__string__")][1], cls, name, field)
                        elif kind == G.SLOT_INT:
                            integer(v, cls, name, field)
                        elif kind == G.SLOT_F32:
                            real(v, cls, name, field)
                        else:
                            string(v[0], G.CAPS[("*", "__string__")][1], cls, name, field)
                            real(v[1], cls, name, field)
                elif kind in (G.RES, G.DEPEND):
                    if len(value) > G.CAPS[(cls, field)][1]:
                        bad(cls, name, field, "too many entries")
                    for v in value:
                        if kind == G.RES:
                            string(v[0], G.CAPS[("*", "__string__")][1], cls, name, field)
                            integer(v[1], cls, name, field)
                        else:
                            string(v, G.CAPS[("*", "__string__")][1], cls, name, field)
                elif kind == G.AREA:
                    if len(value) != 10 or any(len(row) != 10 for row in value):
                        bad(cls, name, field, "invalid AREA shape")
                    for row in value:
                        for v in row:
                            integer(v, cls, name, field)
                elif kind == G.BANK_RANGE:
                    if any(not isinstance(v, int) or not 0 <= v < 100 for v in value):
                        bad(cls, name, field, "bank index outside 0..99")
                elif kind == G.SOURCE4:
                    if len(value) != 4:
                        bad(cls, name, field, "expected four values")
                    for v in value:
                        integer(v, cls, name, field)
                target = G.REF.get((cls, field))
                if target:
                    if kind == G.STR:
                        refs = [value]
                    elif kind == G.SLOT_STR:
                        refs = value.values()
                    elif kind == G.CAP:
                        refs = [v[0] for v in value.values()]
                    elif kind == G.RES:
                        refs = [v[0] for v in value]
                    elif kind == G.DEPEND:
                        refs = value
                    else:
                        refs = []
                    if target == "upgrade_object":
                        target = "unit" if definitions.get(rec["type"], 0) == 1 else "weapon"
                    for ref in refs:
                        if ref and ref not in pools[target]:
                            bad(cls, name, field, f"unknown {target} reference {ref!r}")
                        if ref and target == "resource_define":
                            bound = G.DOMAIN[(cls, field)]
                            if not bound[0] <= definitions[ref] <= bound[1]:
                                bad(cls, name, field, f"resource ID {definitions[ref]} outside {bound}")
                if (cls, field) in G.DOMAIN and kind not in (G.RES, G.CAP):
                    lo, hi = G.DOMAIN[(cls, field)]
                    resolved = definitions.get(value, 0) if kind == G.STR else value
                    if not lo <= resolved <= hi:
                        bad(cls, name, field, f"resolved value {resolved} outside {lo}..{hi}")
            if cls == "BUILDING":
                kind = definitions.get(rec["type"], 0) & 0xff
                slots = rec["extract_fuel"]
                mines = {value for role, value in G.BUILDING_KINDS if role == "mine"}
                fuel_types = {value for role, value in G.BUILDING_KINDS if role in ("port", "mother", "shuttle")}
                capacities = rec["capacity"]
                full_cap_types = {value for role, value in G.BUILDING_KINDS if role in ("mine", "silo")}
                short_cap_types = {value for role, value in G.BUILDING_KINDS if role in ("mother", "port")}
                capacity_slots = range(10) if kind in full_cap_types else range(8) if kind in short_cap_types else ()
                for slot in capacity_slots:
                    value = capacities.get(slot, ("", 0.0))[1]
                    if value and not -2147483648 <= math.trunc(value) <= 2147483647:
                        bad(cls, name, "capacity", "capacity value outside int32 conversion")
                if kind in mines:
                    sums = {}
                    accepted = 0
                    for slot in range(10):
                        resource, value = slots.get(slot, ("", 0.0))
                        if value == 0 or accepted == 4:
                            continue
                        accepted += 1
                        if not -2147483648 <= math.trunc(value) <= 2147483647:
                            bad(cls, name, "extract_fuel", "mine value outside int32 conversion")
                        rid = definitions.get(resource, 0)
                        combined = sums.get(rid, 0) + value
                        if not math.isfinite(combined) or not -2147483648 <= math.trunc(combined) <= 2147483647:
                            bad(cls, name, "extract_fuel", "mine accumulator outside int32")
                        sums[rid] = math.trunc(combined)
                elif kind in fuel_types:
                    for slot in range(4):
                        # The slot-3 fuel branch consumes slot-4's value.
                        value = slots.get(4 if slot == 3 else slot, ("", 0.0))[1]
                        if not -2147483648 <= math.trunc(value) <= 2147483647:
                            bad(cls, name, "extract_fuel", "fuel value outside int32 conversion")
            if cls == "UPGRADE":
                for field in ("step_speed", "turn_speed"):
                    value = math.trunc(rec[field])
                    if not -2147483648 <= value <= 2147483647:
                        bad(cls, name, field, "upgrade speed outside int32 conversion")
            if cls == "ANIM" and (rec["length"] < 0 or rec["repeat"] < 0):
                bad(cls, name, "length", "negative animation length/repeat")
    if sum(r["length"] * r["repeat"] for r in model.classes["ANIM"]) > G.CAPS[("ANIM", "__pool__")][1]:
        bad("ANIM", "*", "length", "animation pool exhausted")
    # The first 16 cursor frames have already been allocated before CFG.
    cursor = 17
    frames = {}
    anim_starts = {}
    for anim in model.classes["ANIM"]:
        name = anim["__name__"]
        anim_starts.setdefault(name, cursor)
        length, repeat, step = anim["length"], anim["repeat"], anim["step"]
        sprite = sprite_ids.get(anim["sprite"], 0)
        end_sprite = sprite + step * (length * repeat - 1)
        if not 0 <= min(sprite, end_sprite) or max(sprite, end_sprite) >= G.CAPS[("SPRITE", "__id__")][1]:
            bad("ANIM", name, "sprite", "generated sprite sequence outside ID space")
        for _ in range(repeat):
            for i in range(length):
                frames[cursor + 1] = [anim["time"], 1 if i + 1 < length else (1 - length if anim["cyclic"] else 0)]
                cursor += 1
    construction_chains = []
    nonconstructible = {value for role, value in G.BUILDING_KINDS if role == "nonconstructible"}
    tick_strict = {value for role, value in G.BUILDING_KINDS if role == "tick_strict"}
    for building in model.classes["BUILDING"]:
        name = building["__name__"]
        kind = definitions.get(building["type"], 0) & 0xff
        anim_name = building["anims"].get(0, "")
        if not anim_name or anim_name not in anim_starts:
            bad("BUILDING", name, "anims", "missing construction animation")
        index = anim_starts[anim_name]
        visited, chain = set(), []
        while True:
            index += 1
            if index in visited or index not in frames:
                bad("BUILDING", name, "anims", "cyclic or invalid construction animation")
            visited.add(index)
            frame = frames[index]
            chain.append(frame)
            jump = frame[1]
            if not jump:
                break
            index += jump - 1
        construction_chains.append((name, kind, chain))
        duration = sum(frame[0] for frame in chain)
        if not math.isfinite(duration) or duration <= 0:
            bad("BUILDING", name, "anims", "nonpositive construction animation duration")
        mul = model.globals.get("BUILDING_BUILD_TIME_MUL", G.MODIFIER_DEFAULTS["BUILDING_BUILD_TIME_MUL"])
        add = model.globals.get("BUILDING_BUILD_TIME_ADD", G.MODIFIER_DEFAULTS["BUILDING_BUILD_TIME_ADD"])
        desired = building["build_time"] * mul + add
        if not math.isfinite(desired) or desired < 0:
            bad("BUILDING", name, "build_time", "negative or nonfinite construction retime")
        factor = desired / duration
        for frame in chain:
            frame[0] *= factor
            if not math.isfinite(frame[0]):
                bad("BUILDING", name, "anims", "nonfinite retimed animation frame")
    # Shared frames can have been retimed again by a later building. Ordinary
    # constructible records need a positive total for the progress divisor.
    for name, kind, chain in construction_chains:
        duration = sum(frame[0] for frame in chain)
        if not math.isfinite(duration) or duration < 0 or any(not math.isfinite(f[0]) for f in chain):
            bad("BUILDING", name, "anims", "negative or nonfinite final construction duration")
        if kind not in nonconstructible and duration == 0:
            bad("BUILDING", name, "anims", "nonpositive constructible animation duration")
    # Online loops cache frame time and subtract it on every iteration.
    # Strict callbacks do not guard zero; guarded callbacks return for zero
    # but still cannot advance when time is negative.
    for building in model.classes["BUILDING"]:
        name = building["__name__"]
        kind = definitions.get(building["type"], 0) & 0xff
        count = building["sprite_quantity"]
        slots = [(slot, True) for slot in range(1, count + 1)] if kind in tick_strict else []
        for role, value in G.BUILDING_KINDS:
            if value != kind:
                continue
            if role.startswith("strict_count_slot_"):
                slot = int(role.removeprefix("strict_count_slot_"))
                if slot <= count:
                    slots.append((slot, True))
            elif role.startswith("guarded_count_slot_"):
                slot = int(role.removeprefix("guarded_count_slot_"))
                if slot <= count:
                    slots.append((slot, False))
            elif role.startswith("guarded_slot_"):
                slots.append((int(role.removeprefix("guarded_slot_")), False))
        for slot, strict in slots:
            anim_name = building["anims"].get(slot, "")
            if not anim_name:
                continue
            index = anim_starts[anim_name]
            visited = set()
            while True:
                index += 1
                if index in visited:
                    break
                if index not in frames:
                    bad("BUILDING", name, "anims", f"invalid online animation slot {slot}")
                visited.add(index)
                time, jump = frames[index]
                if not math.isfinite(time) or time < 0 or (strict and time == 0):
                    bad("BUILDING", name, "anims", f"invalid online animation time in slot {slot}")
                if not jump:
                    break
                index += jump - 1


def main(argv=None):
    import argparse
    from pathlib import Path
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--init", required=True, type=Path)
    parser.add_argument("--lang", required=True, type=Path)
    parser.add_argument("--locale", required=True)
    parser.add_argument("--input", nargs=3, action="append", default=[],
                        metavar=("MOD_INDEX", "MOD_PATH", "FILE"))
    parser.add_argument("--out-init", required=True, type=Path)
    parser.add_argument("--out-lang", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        init, lang = args.init.read_bytes(), args.lang.read_bytes()
        inputs = [Input(file, path, Path(file).read_bytes(), int(index))
                  for index, path, file in args.input]
        out = prepare(init, lang, inputs, args.locale)
        if out is None:
            out = init, lang
        args.out_init.write_bytes(out[0])
        args.out_lang.write_bytes(out[1])
    except (OSError, ValueError) as exc:
        parser.exit(1, f"{exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
