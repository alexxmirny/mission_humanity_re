"""English <-> wire alias layer (Phase 4b).

The YAML sources are meant to read in English, but the game only knows the
original (Polish/Russian) wire names — the exe references ~31 of them as string
literals, saves persist object indices, and every cross-reference is by name.
This module owns the one-to-one mapping between the two, so the editable sources
can use English identifiers while the compiled cfg stays byte-for-byte
game-safe.

Design
------
- **One global bijection.** ``aliases.yaml`` is *organised* into per-namespace
  sections (``units``, ``buildings``, ``weapons``, …) for readability and for the
  bootstrap/coverage tooling, but at load time every ``wire -> english`` entry is
  merged into a single global map (and its inverse). Any collision — a wire name
  aliased twice, or one English name reused for two wire names — is an error,
  because the reverse map must be unambiguous for the round-trip to be lossless.
  Union reference universes (``art`` = sprites|anims, ``invention`` =
  define|progress, …) then need no special handling: a ref token is just looked
  up in the one map.
- **Passthrough.** A name with no alias translates to itself, in both
  directions. So a partial table (the machinery-first seed) is valid and its
  compiled output is identical to the no-alias output.
- **Grammar-driven substitution.** :func:`to_english`/:func:`to_wire` rebuild a
  :class:`~cfgkit.parse.Model` translating exactly the name-bearing positions:
  every class object ``__name__``; the ``defines``/``def_banks``/``icons``/
  ``texts`` keys, ``BANK`` names and ``SPRITE`` names; and each class field that
  is a cross-reference (``grammar.REF``) or a resource list (``RES`` — the ore
  names). Non-name strings (``PLANET.map`` filenames, ``TEXT`` values) are never
  touched.

Round-trip contract (the Phase-4b gate): for retail, aliases are invisible on
the wire — ``emit(to_wire(to_english(m)))`` is byte-identical to ``emit(m)``.
"""
from __future__ import annotations

import os

import yaml

from . import grammar as G
from .emit import _reverse_map
from .parse import Model

# Reference namespaces, one per name-definition site. The class namespaces map
# 1:1 to a model class; the flat ones (defines/def_banks/icons/text_ids/banks/
# sprites) map to a top-level model list; ``resources`` is reference-only (the
# ore names used in RES fields — they have no standalone definition site).
CLASS_NS = {
    "UNIT": "units", "BUILDING": "buildings", "WEAPON": "weapons", "ANIM": "anims",
    "PROGRESS": "inventions", "UPGRADE": "upgrades", "PROJECT": "projects",
    "PLANET": "planets", "SYSTEM": "systems", "TREE": "trees", "STONE": "stones",
}
FLAT_NS = ["defines", "def_banks", "icons", "text_ids", "banks", "sprites"]
NAMESPACES = FLAT_NS + list(CLASS_NS.values()) + ["resources"]


class AliasError(ValueError):
    """A malformed alias table (unknown namespace, collision, or dangling ref)."""


# --- name-bearing field discovery (per class) -------------------------------

def _name_fields(cls):
    """{field: kind} for class fields whose value(s) are namespace names:
    every cross-reference field (grammar.REF) plus resource lists (RES ore
    names). Other STR fields (e.g. PLANET.map filenames) are excluded."""
    rev = _reverse_map(cls)
    out = {}
    for field, (kind, _kw) in rev.items():
        if (cls, field) in G.REF or kind == G.RES:
            out[field] = kind
    return out


def _xlate_field(kind, val, mp):
    """Translate the name token(s) inside one field value through *mp*
    (passthrough for unmapped names). Returns a fresh container."""
    if kind == G.STR:
        return mp.get(val, val)
    if kind == G.SLOT_STR:
        return {s: mp.get(n, n) for s, n in val.items()}
    if kind == G.CAP:
        return {s: (mp.get(n, n), v) for s, (n, v) in val.items()}
    if kind == G.DEPEND:
        return [mp.get(n, n) for n in val]
    if kind == G.RES:
        return [(mp.get(n, n), v) for n, v in val]
    return val  # a REF field of a non-name kind (none today) — leave untouched


def _translate(model, mp):
    """Rebuild *model* with every name-bearing token mapped through *mp*."""
    g = mp.get
    m = Model()
    m.defines = [(g(n, n), v) for n, v in model.defines]
    m.def_banks = [(g(n, n), v) for n, v in model.def_banks]
    m.icons = [(g(n, n), v) for n, v in model.icons]
    m.texts = [(g(k, k), v) for k, v in model.texts]
    m.banks = [{"name": g(b["name"], b["name"]),
                "sprites": [(g(n, n), f) for n, f in b["sprites"]]}
               for b in model.banks]
    m.globals = dict(model.globals)
    m.issues = list(model.issues)
    for cls, recs in model.classes.items():
        nf = _name_fields(cls)
        for rec in recs:
            r = dict(rec)
            r["__name__"] = g(rec["__name__"], rec["__name__"])
            for field, kind in nf.items():
                if field in r:
                    r[field] = _xlate_field(kind, r[field], mp)
            m.classes[cls].append(r)
    return m


# --- model name universe (for dangling / coverage checks) -------------------

def model_names(model):
    """{namespace: set(wire names)} actually present in *model*."""
    u = {ns: set() for ns in NAMESPACES}
    u["defines"].update(n for n, _ in model.defines)
    u["def_banks"].update(n for n, _ in model.def_banks)
    u["icons"].update(n for n, _ in model.icons)
    u["text_ids"].update(k for k, _ in model.texts)
    for b in model.banks:
        u["banks"].add(b["name"])
        u["sprites"].update(n for n, _ in b["sprites"])
    for cls, recs in model.classes.items():
        ns = CLASS_NS[cls]
        nf = _name_fields(cls)
        for rec in recs:
            u[ns].add(rec["__name__"])
            for field, kind in nf.items():
                if field not in rec:
                    continue
                if kind == G.RES:
                    u["resources"].update(n for n, _ in rec[field])
                # ref names into other namespaces are captured at their
                # definition site above; RES ore names have no def site, so we
                # harvest them here.
    return u


# --- the table --------------------------------------------------------------

class AliasTable:
    """A validated, bidirectional wire<->English name map."""

    def __init__(self, sections):
        self.sections = {ns: dict(sections.get(ns) or {}) for ns in NAMESPACES}
        self.wire2en, self.en2wire = {}, {}
        for ns, m in self.sections.items():
            for wire, en in m.items():
                wire, en = str(wire), str(en)
                if wire in self.wire2en and self.wire2en[wire] != en:
                    raise AliasError(f"wire name {wire!r} aliased twice "
                                     f"({self.wire2en[wire]!r} vs {en!r})")
                if en in self.en2wire and self.en2wire[en] != wire:
                    raise AliasError(f"English name {en!r} used for two wire names "
                                     f"({self.en2wire[en]!r} and {wire!r})")
                self.wire2en[wire] = en
                self.en2wire[en] = wire

    def __len__(self):
        return len(self.wire2en)

    @classmethod
    def load(cls, path):
        """Load ``aliases.yaml``; an absent file yields an empty (all-passthrough)
        table so the alias layer is opt-in."""
        if not path or not os.path.exists(path):
            return cls({})
        doc = yaml.safe_load(open(path, encoding="utf-8")) or {}
        unknown = set(doc) - set(NAMESPACES)
        if unknown:
            raise AliasError(f"{path}: unknown namespace(s) {sorted(unknown)}; "
                             f"valid: {NAMESPACES}")
        return cls(doc)

    def dump(self, path):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        body = {ns: self.sections[ns] for ns in NAMESPACES if self.sections[ns]}
        with open(path, "w", encoding="utf-8") as f:
            f.write("# GENERATED alias seed (Phase 4b) — wire name -> English "
                    "source name.\n"
                    "# Curate freely; every English name must be unique. Names "
                    "with no entry\n# pass through unchanged. Recompile after "
                    "editing.\n\n")
            yaml.safe_dump(body, f, allow_unicode=True, sort_keys=False, width=100)

    def to_english(self, model):
        return _translate(model, self.wire2en)

    def to_wire(self, model):
        return _translate(model, self.en2wire)

    def check(self, model):
        """Dangling + collision + coverage report against *model*.

        Returns ``(errors, info)`` — errors are dangling aliases (a wire name in
        the table that no longer exists) and English names that collide with an
        *unaliased* wire name (which would make the reverse map ambiguous); info
        is per-namespace coverage counts.
        """
        universe = model_names(model)
        all_wire = set().union(*universe.values())
        errors, info = [], []
        for ns in NAMESPACES:
            uni, tbl = universe[ns], self.sections[ns]
            dangling = sorted(w for w in tbl if w not in uni)
            for w in dangling:
                errors.append(f"{ns}: alias for {w!r} but no such name in the config")
            # An English name that is *also* a wire name mapped elsewhere (i.e.
            # not aliased to itself) breaks the round-trip: that wire name passes
            # through to the same English string, so two names collide and
            # to_wire can't tell them apart.
            for w, en in tbl.items():
                if en in all_wire and self.wire2en.get(en) != en:
                    errors.append(f"{ns}: English name {en!r} collides with the "
                                  f"wire name {en!r}")
            named = sum(1 for n in uni if n in self.wire2en)
            info.append(f"{ns:12} {named:>4}/{len(uni):<4} named")
        return errors, info


# --- bootstrap: a draft table from initlang glosses + resource seed ---------

_RESOURCE_SEED = {f"zloze_{c}": f"ore_{c}" for c in "abcd"}


def _slug(text, maxlen=44):
    """A snake_case identifier candidate from free English text."""
    out = []
    for ch in text.lower():
        out.append(ch if ch.isalnum() else "_")
    s = "_".join(p for p in "".join(out).split("_") if p)[:maxlen].strip("_")
    if s and s[0].isdigit():
        s = "t_" + s
    return s


def _read_glosses(lang_path):
    """[(TEXT key, English comment)] in file order — the ``//`` comment that
    immediately precedes each translatable string in initlang.cfg."""
    raw = open(lang_path, "rb").read().decode("utf-16-le")
    last_comment = None
    out = []
    for line in raw.splitlines():
        s = line.strip()
        if s.startswith("//"):
            c = s[2:].strip().strip('"').strip()
            if c:
                last_comment = c
            continue
        if s.startswith("TEXT"):
            try:
                key = s.split('"', 2)[1]      # TEXT "KEY" "value"
            except IndexError:
                last_comment = None
                continue
            out.append((key, last_comment))
            last_comment = None
    return out


class _Alloc:
    """Hand out unique English names, never colliding with a reserved set of
    wire names (which would break the round-trip) or with a name already given.
    Deterministic: clashes get ``_2``, ``_3``, … suffixes."""

    def __init__(self, reserved):
        self.taken = set(reserved)

    def give(self, base):
        cand, n = base, 1
        while cand in self.taken:
            n += 1
            cand = f"{base}_{n}"
        self.taken.add(cand)
        return cand


def bootstrap(model, lang_path=None):
    """A machinery-first draft :class:`AliasTable`: text ids get English-gloss
    slugs, the four ore resources get ``ore_a..d``, everything else passes
    through (curate names later). Every generated name is checked against the
    full wire-name universe so the table is a clean bijection by construction."""
    universe = model_names(model)
    all_wire = set().union(*universe.values())
    sections = {ns: {} for ns in NAMESPACES}

    alloc = _Alloc(all_wire)
    sections["resources"] = {}
    for w, e in _RESOURCE_SEED.items():
        if w in universe["resources"]:
            sections["resources"][w] = alloc.give(e)

    if lang_path:
        tids = {}
        for key, comment in _read_glosses(lang_path):
            if key not in universe["text_ids"]:
                continue
            slug = _slug(comment) if comment else ""
            if slug:
                tids[key] = alloc.give(slug)
        sections["text_ids"] = tids

    return AliasTable(sections)
