"""Wiki view-models: game data (INIT.CFG + INITLANG.CFG + INFO.TXT) -> plain dataclasses.

Everything a Jinja template needs is a plain attribute / dict / list here; nothing in the model
touches the filesystem or a template.  Build one with::

    from src.formats.wiki import source, model
    wiki = model.build(source.load(en_install, ru_install))

Terminology: ENERGY is the HP-like stat (units' hit points, buildings' hit points / build-up);
POWER is the generated resource (power plants make it, buildings need it).  They are different.

Schema
------
``Wiki``
    ``langs``                 ordered language codes (``["en", "ru"]``); ``"en"`` is the fallback
    ``units / buildings / projects / weapons / planets / systems / upgrades``
                              entity lists in cfg order (upgrades have no pages of their own)
    ``by_kind``               ``{"unit": [...], ...}``; ``get(kind, slug)``; ``by_cfg(kind, name)``
    ``inventions``            ``{invention name: Invention}`` (the PROGRESS graph, cfg order)
    ``tech_graph(faction)``   nodes + AND edges for the tech-tree writer (see below)
    ``label(lang, key)``      TEXT value for a ``K_*`` key, EN fallback (``text(lang, key)`` too)
    ``target_letters(lang, mask)``  weapon target letters from K_MENU_GA ("G", "A", "GA")
    ``missing[lang]``         sorted keys the model used that the language lacks (EN is used)
    ``mods``                  the global ADD/MUL modifiers read from the cfg
    ``counts()``              ``{"unit": 54, ...}``

``Entity`` (all page entities; ``Unit/Building/Project/Weapon/Planet/System/Upgrade`` subclass it)
    ``kind``       "unit" | "building" | "project" | "weapon" | "planet" | "system" | "upgrade"
    ``cfg_name``   the cfg object name (``N_human_Soldier3``)
    ``slug``       cfg name lower-cased, ``N_`` dropped, unique inside the kind
    ``faction``    "alien" | "human" | None (planets, systems; weapons are derived from their users,
                   None when both factions or nobody use them)
    ``names``      ``{lang: str}`` (TEXT[cfg name]; EN fallback; upgrades are named after their project)
    ``icon``       cfg ICON name, ``icon_slot`` its slot number (None when unknown); art keys in ``art``
    ``art``        ``{field: value}`` of every art/sprite/anim key a renderer needs
    ``description`` ``{lang: str}`` from INFO.TXT (``description_fallback`` = langs that fell back to EN)
    ``panel``      ``Panel`` (the in-game info screen); ``extra``: ``Row`` list of cfg fields the game
                   does not show (``shown=False``, note "config, not shown in game")
    ``links``      ``{role: [Link, ...]}`` cross-links (roles per kind below)
    ``invention``  invention name (units, buildings, projects, upgrades, planets, systems)
    ``raw``        every non-default cfg field, JSON-friendly (raw values, before global modifiers)

``Row`` (NamedTuple; the first four fields are the design tuple ``(label_key, value, unit_key, marker)``)
    ``label_key``  K_* TEXT key (alien ``_a`` variant already chosen), or None for ``extra`` rows
    ``value``      number (already rounded half up with the in-game formula) or None
    ``unit_key``   K_* key of the unit text ("m", "km/h", "s", "km/s") or None
    ``marker``     "" | "+" | "-" (upgrade rows) -- value is stored as an absolute number
    ``verified``   False = best reading of an UNVERIFIED design item (see list below)
    ``note``       free text for tooltips ("HP, not the power resource", "wiki addition", ...)
    ``items``      ``[Item(label_key, link, value)]`` for list rows (ore rows, unit lists)
    ``cfg_key``    cfg keyword (extra rows)    ``shown`` False for extra rows

``Panel``
    ``left``       [Row]  stat rows: units (speed, energy, sight, production time), buildings
                   (power, energy, people/workers, build time, flight speed)
    ``right``      [Row]  building right column (by building TYPE) / project rows
    ``weapons``    [WeaponRow(weapon: Link, slot, power, range_min, range_max, target_mask)]
    ``costs_label_key``/``costs``  "Costs:" + [Cost(label_key, value)] x4 (ore order), or None
    ``upgrade``    building only: {"target": Link, "project": Link|None, "panel": Panel} or None
    ``offers``     project only: [Offer(role, label_key, link, panel, affected)]
                   roles: building_new | building_upgrade | unit | upgrade_unit | upgrade_weapon
    ``research``   project only: Row with the research time (wiki addition, not shown in game)

``Link(kind, slug, role)``  -> ``wiki.get(kind, slug)``.  Link roles by kind:
    unit      weapons produced_by transported_by equivalent research upgrades requires unlocks
    building  produces transports weapon upgrade_to upgrade_from equivalent research requires unlocks
    project   requires unlocks (everything whose invention depends on this project) offers
    weapon    used_by_units used_by_buildings upgrades
    planet    system requires unlocks        system  planets requires unlocks
    upgrade   project affects

Invention graph (for the tech tree)
-----------------------------------
``wiki.inventions[name]`` -> ``Invention(name, type, faction, owner: Link|None, page: Link|None,
depends: [name], dependents: [name])``; ``depends`` is an AND list (vanilla has no OR).
``wiki.tech_graph(faction=None)`` -> ``{"nodes": [...], "edges": [(prerequisite, dependent), ...]}``:
nodes are dicts ``{id, type, faction, owner, page, icon, icon_slot, names, depends}`` in cfg order;
``faction`` "alien"/"human" keeps that faction's inventions plus any neutral ones (planets, systems)
they need (transitively); ``None`` keeps everything.  Deterministic; every invention appears once;
edges only join included nodes (no dangling ids).

Formulas (round half up everywhere)
-----------------------------------
x' = x * MUL + ADD with the cfg's global modifiers.  Unit speed = K / step_speed' (K = 9.6 planes,
3.2 helicopters, 1.6 ground vehicles and walkers, 0.8 the rest), sight = (sight' ) * 50 m,
energy = energy', production time = build_time * UNIT_BUILD_TIME_MUL + UNIT_BUILD_TIME_MUL (retail
bug: MUL where ADD belongs).  Building power = (electric_power + BUILDING_ENERGY_ADD) *
BUILDING_POWER_MUL (retail bug: the energy ADD), energy = energy', build time = build_time',
flight speed = 1000 / velocity.

UNVERIFIED (best reading implemented, ``Row.verified == False``)
----------------------------------------------------------------
building build time uses the modified value; EXTRACT/CAPACITY/FUEL float -> int rounding; upgrade
row semantics (unit upgrade "Speed"/"Armor" = step_speed/energy deltas, weapon upgrade has no strike
range because UPGRADE carries no fire_range); "possibility of space flight" shown only when ABILITY
is set; flight speed for shuttles only when ABILITY; unit speed K for the non-plane/heli flying
types; "Max workers" = HUMAN (BUILDER is the alternative); unit cost modifier UNIT_RESOURCE.
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass, field
from typing import NamedTuple

from . import source as _source

# --------------------------------------------------------------------------------- constants

SIGHT_UI_MUL = 50.0  # G_SIGHT_UI_MUL @0x50343e
SPEED_K = {"plane": 9.6, "heli": 3.2, "ground": 1.6, "other": 0.8}
STORAGE_UNITS = 50  # "Units storage" is hardcoded in the game (PARAMETR is dead in retail)
RES_ORDER = ("zloze_a", "zloze_b", "zloze_c", "zloze_d")  # Extract, Iron, Carbon, Titanium
RES_LABEL = (
    "K_CONS_RESOURCE",
    "K_CONS_RESOURCE1",
    "K_CONS_RESOURCE2",
    "K_CONS_RESOURCE3",
)
NOT_SHOWN = "config, not shown in game"

# building TYPE number (alien 1..18, human 21..38) -> role
B_FACTORY, B_MINE, B_PLANT, B_COLONY, B_TURRET, B_MOTHER = 1, 2, 3, 4, 5, 6
B_PORT, B_SHUTTLE, B_STORAGE = 12, 13, 16
B_STORE_UNITS = (7, 8, 9, 10)

# Buildings nobody can construct in a normal game: TYPE role 14 (main base "Baza") and
# 18 (civil buildings "Chatka", 8 map decorations per faction).  Evidence: their cfg records have
# no RESOURCE cost, no BUILD_TIME and no construction animation (every constructible building
# has an ``A_*_budowa`` slot-0 anim); the base has ENERGY 999999 (indestructible scenery).
UNBUILDABLE_ROLES = {14: "main base", 18: "civil building"}

STARMAP_W, STARMAP_H, STARMAP_PAD = 1024, 768, 32  # galaxy screen size; planet widget padding
STARMAP_ANIM = 54  # planet animation frame size (px)

KINDS = ("unit", "building", "project", "weapon", "planet", "system", "upgrade")


def rnd(x) -> int:
    """Round half up (the game's display rounding); epsilon absorbs float32 representation noise."""
    return math.floor(x + 0.5 + 1e-9)


# --------------------------------------------------------------------------------- data types


class Link(NamedTuple):
    kind: str
    slug: str
    role: str = ""


class Item(NamedTuple):
    label_key: str | None
    link: Link | None
    value: float | None


class Cost(NamedTuple):
    label_key: str
    value: int


class Row(NamedTuple):
    label_key: str | None
    value: object
    unit_key: str | None = None
    marker: str = ""
    verified: bool = True
    note: str = ""
    items: tuple = ()
    cfg_key: str = ""
    shown: bool = True


@dataclass
class WeaponRow:
    weapon: Link
    slot: int
    power: int
    range_min: int
    range_max: int
    target_mask: int  # bit 1 = ground, bit 2 = air


@dataclass
class Offer:
    role: str
    label_key: str | None
    link: Link
    panel: Panel | None = None
    affected: list = field(default_factory=list)


@dataclass
class Panel:
    left: list = field(default_factory=list)
    right: list = field(default_factory=list)
    weapons: list = field(default_factory=list)
    costs_label_key: str | None = None
    costs: list = field(default_factory=list)
    upgrade: dict | None = None
    offers: list = field(default_factory=list)
    research: Row | None = None


@dataclass
class Invention:
    name: str
    type: str  # cfg PROGRESS TYPE: budynek|pojazd|projekt|planeta|uklad|upgrade
    faction: str | None
    owner: Link | None
    page: Link | None
    depends: list = field(default_factory=list)
    dependents: list = field(default_factory=list)


@dataclass
class Entity:
    kind: str = ""
    cfg_name: str = ""
    slug: str = ""
    faction: str | None = None
    names: dict = field(default_factory=dict)
    icon: str = ""
    icon_slot: int | None = None
    art: dict = field(default_factory=dict)
    description: dict = field(default_factory=dict)
    description_fallback: list = field(default_factory=list)
    panel: Panel = field(default_factory=Panel)
    extra: list = field(default_factory=list)
    links: dict = field(default_factory=dict)
    invention: str = ""
    raw: dict = field(default_factory=dict)


@dataclass
class Unit(Entity):
    unit_class: str = ""  # soldier|vehicle|walker|plane|helicopter|mothership|ship|other
    soldier_count: int = 0  # 1..5 for the Soldier1..5 / Insect stacks, else 0
    speed_k: float = 0.0
    hide_production: bool = False  # heli-motherships show no production time / costs


@dataclass
class Building(Entity):
    btype: int = 0  # raw cfg TYPE number
    brole: int = 0  # 1..18 (alien/human folded)
    role_name: str = ""  # factory|mine|plant|colony|turret|mother|garage|barracks|...
    # upgrade chain (cfg UPGRADE -> next building): the base building and its upgrades are ONE page
    level: int = 0  # 0 = base building, 1.. = upgrade level
    chain: list = field(default_factory=list)  # [Link] base first; the same list on every member
    chain_base: Link | None = None
    buildable: bool = True  # False: main base / civil building, never constructible in play
    unbuildable_why: str = ""


@dataclass
class Project(Entity):
    pass


@dataclass
class Weapon(Entity):
    target_mask: int = 0


@dataclass
class Planet(Entity):
    pass


@dataclass
class System(Entity):
    pass


@dataclass
class Upgrade(Entity):
    utype: str = ""  # "unit" | "weapon"
    objects: list = field(default_factory=list)  # [Link] in slot order


_CLASS = {
    "unit": Unit,
    "building": Building,
    "project": Project,
    "weapon": Weapon,
    "planet": Planet,
    "system": System,
    "upgrade": Upgrade,
}
_CFG_CLASS = {
    "UNIT": "unit",
    "BUILDING": "building",
    "PROJECT": "project",
    "WEAPON": "weapon",
    "PLANET": "planet",
    "SYSTEM": "system",
    "UPGRADE": "upgrade",
}

_BROLE_NAME = {
    1: "factory",
    2: "mine",
    3: "plant",
    4: "colony",
    5: "turret",
    6: "mother",
    7: "garage",
    8: "barracks",
    9: "hangar",
    10: "helipad",
    11: "laboratory",
    12: "port",
    13: "shuttle",
    14: "base",
    15: "relay",
    16: "storage",
    17: "office",
    18: "civil",
}


# --------------------------------------------------------------------------------- helpers


def _jsonable(v):
    if isinstance(v, set):
        return sorted(v)
    if isinstance(v, dict):
        return {str(k): _jsonable(x) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [_jsonable(x) for x in v]
    return v


def _empty(v):
    return v in (0, "", None, 0.0) or (isinstance(v, (list, dict, set, tuple)) and not v)


def _slug_base(cfg_name: str) -> str:
    s = cfg_name.removeprefix("N_")
    return s.lower()


_FACTION_RE = re.compile(r"(?:^|_)(alien|human)(?:_|$)", re.IGNORECASE)


def _faction_of(*names):
    for n in names:
        m = _FACTION_RE.search(n or "")
        if m:
            return m.group(1).lower()
    return None


class Mods:
    """The cfg's 24 global ADD/MUL modifiers (defaults: ADD 0, MUL 1)."""

    def __init__(self, g):
        self.g = dict(g)

    def mul(self, stem):
        return float(self.g.get(stem + "_MUL", 1.0))

    def add(self, stem):
        return float(self.g.get(stem + "_ADD", 0))

    def apply(self, stem, v):
        return v * self.mul(stem) + self.add(stem)


# --------------------------------------------------------------------------------- the model


class Wiki:
    def __init__(self, data):
        self.data = data
        self.langs = list(data.langs)
        if "en" in self.langs:  # EN first: it is the fallback
            self.langs.remove("en")
            self.langs.insert(0, "en")
        self.cfg = _source.parse_cfg(data.init)
        self.defines = dict(self.cfg.defines)
        self.mods = Mods(self.cfg.globals)
        self.texts = {}
        for lang in self.langs:
            t = dict(self.cfg.texts)
            t.update(_source.parse_cfg(data.langs[lang].initlang).texts)
            self.texts[lang] = t
        self.icon_slots = dict(self.cfg.icons)
        self.by_kind = {k: [] for k in KINDS}
        self._cfg = {k: {} for k in KINDS}
        self._slugs = {k: {} for k in KINDS}
        self._used = set()
        self._name_keys = set()
        self.inventions = {}
        self._inv_owner = {}
        self._build()

    # ------------------------------------------------------------------ lookups

    def get(self, kind, slug):
        return self._slugs[kind][slug]

    def by_cfg(self, kind, cfg_name):
        return self._cfg[kind].get(cfg_name)

    def counts(self):
        return {k: len(v) for k, v in self.by_kind.items()}

    @property
    def units(self):
        return self.by_kind["unit"]

    @property
    def buildings(self):
        return self.by_kind["building"]

    @property
    def projects(self):
        return self.by_kind["project"]

    @property
    def weapons(self):
        return self.by_kind["weapon"]

    @property
    def planets(self):
        return self.by_kind["planet"]

    @property
    def systems(self):
        return self.by_kind["system"]

    @property
    def upgrades(self):
        return self.by_kind["upgrade"]

    def text(self, lang, key):
        """TEXT value for ``key`` in ``lang`` with EN fallback ('' when no language has it)."""
        t = self.texts.get(lang, {})
        if key in t:
            return t[key]
        return self.texts.get("en", {}).get(key, "")

    label = text

    def target_letters(self, lang, mask):
        """Target letters of a weapon row: first char of K_MENU_GA if ground, second if air."""
        ga = self.text(lang, "K_MENU_GA")
        out = ""
        if mask & 1 and len(ga) > 0:
            out += ga[0]
        if mask & 2 and len(ga) > 1:
            out += ga[1]
        return out

    # ------------------------------------------------------------------ construction

    def _lk(self, base, faction):
        """Label key with the alien ``_a`` variant when the language files define one."""
        key = (
            base + "_a"
            if faction == "alien" and (base + "_a") in self.texts.get("en", {})
            else base
        )
        self._used.add(key)
        return key

    def _key(self, key):
        if key:
            self._used.add(key)
        return key

    def _row(self, label_key, value, unit_key=None, marker="", **kw):
        self._key(label_key)
        self._key(unit_key)
        return Row(label_key, value, unit_key, marker, **kw)

    def _build(self):
        for cfg_cls, kind in _CFG_CLASS.items():
            for rec in self.cfg.classes.get(cfg_cls, []):
                self._add_entity(kind, rec)
        self._build_inventions()
        self._name_entities()
        self._descriptions()
        self._links()
        self._chains()
        self._panels()
        self._weapon_factions()
        self._finish_missing()

    def _add_entity(self, kind, rec):
        e = _CLASS[kind]()
        e.kind = kind
        e.cfg_name = rec["__name__"]
        base = _slug_base(e.cfg_name)
        slug, n = base, 2
        while slug in self._slugs[kind]:
            slug, n = f"{base}_{n}", n + 1
        e.slug = slug
        e.invention = rec.get("invention", "") or ""
        e.faction = _faction_of(e.cfg_name, e.invention) if kind != "weapon" else None
        e.icon = rec.get("icon", "") or ""
        e.icon_slot = self.icon_slots.get(e.icon) if e.icon else None
        e.raw = {k: _jsonable(v) for k, v in rec.items() if k != "__name__" and not _empty(v)}
        e._rec = rec  # cfg record (not part of the public schema)
        self.by_kind[kind].append(e)
        self._slugs[kind][slug] = e
        self._cfg[kind].setdefault(e.cfg_name, e)
        if e.invention:
            self._inv_owner.setdefault(e.invention, e)

    def link(self, e, role=""):
        return Link(e.kind, e.slug, role) if e is not None else None

    # ------------------------------------------------------------------ inventions

    def _build_inventions(self):
        for rec in self.cfg.classes.get("PROGRESS", []):
            name = rec["__name__"]
            owner = self._inv_owner.get(name)
            self.inventions[name] = Invention(
                name=name,
                type=rec.get("type", ""),
                faction=_faction_of(name),
                owner=self.link(owner),
                page=None,
                depends=list(rec.get("depends", [])),
            )
        for inv in self.inventions.values():
            for d in inv.depends:
                if d in self.inventions:
                    self.inventions[d].dependents.append(inv.name)

    # ------------------------------------------------------------------ names, descriptions

    def _text_name(self, lang, cfg_name):
        t = self.texts[lang]
        if cfg_name in t:
            return t[cfg_name]
        self._name_keys.add(cfg_name)
        return self.texts.get("en", {}).get(cfg_name, "")

    def _name_entities(self):
        for kind in KINDS:
            for e in self.by_kind[kind]:
                if kind == "upgrade":
                    continue
                e.names = {
                    lang: self._text_name(lang, e.cfg_name) or e.cfg_name for lang in self.langs
                }
        # upgrades have no TEXT name: take the name of the project that researches them
        for e in self.by_kind["upgrade"]:
            proj = self._research_project_of(e.invention)
            e.names = dict(proj.names) if proj else {lang: e.cfg_name for lang in self.langs}
        # fill the page link of every invention (upgrades point at their project)
        for inv in self.inventions.values():
            owner = self._inv_owner.get(inv.name)
            if owner is None:
                continue
            if owner.kind == "upgrade":
                proj = self._research_project_of(inv.name)
                inv.page = self.link(proj)
            else:
                inv.page = self.link(owner)

    def _research_project_of(self, invention):
        inv = self.inventions.get(invention)
        if not inv:
            return None
        for d in inv.depends:
            o = self._inv_owner.get(d)
            if o is not None and o.kind == "project":
                return o
        return None

    def _descriptions(self):
        for e in (
            list(self.by_kind["unit"])
            + self.by_kind["building"]
            + self.by_kind["project"]
            + self.by_kind["planet"]
        ):
            e.description, e.description_fallback = self._describe(e._rec.get("info_txt", ""))
        for e in self.by_kind["upgrade"]:
            proj = self._research_project_of(e.invention)
            if proj:
                e.description, e.description_fallback = (
                    dict(proj.description),
                    list(proj.description_fallback),
                )

    def _describe(self, info_txt):
        out, fb = {}, []
        key = _source.norm_path(info_txt) if info_txt else ""
        for lang in self.langs:
            info = self.data.langs[lang].info
            if key and key in info:
                out[lang] = info[key]
            elif key and key in self.data.langs.get("en", _source.LangData()).info:
                out[lang] = self.data.langs["en"].info[key]
                fb.append(lang)
            else:
                out[lang] = ""
        return out, fb

    # ------------------------------------------------------------------ links

    def _owner_links(self, names, role=""):
        out = []
        for n in names:
            o = self._inv_owner.get(n)
            if o is not None:
                out.append(self.link(o, role))
        return out

    def _links(self):
        for kind in ("unit", "building", "project", "planet", "system", "upgrade"):
            for e in self.by_kind[kind]:
                inv = self.inventions.get(e.invention)
                e.links.setdefault("requires", self._owner_links(inv.depends) if inv else [])
                e.links.setdefault("unlocks", self._owner_links(inv.dependents) if inv else [])
        # research projects (a unit/building's invention depends on a project invention)
        for kind in ("unit", "building"):
            for e in self.by_kind[kind]:
                e.links["research"] = [lk for lk in e.links["requires"] if lk.kind == "project"]
        for e in self.by_kind["unit"]:
            e.links["weapons"] = []
            for slot in sorted(e._rec.get("weapons", {})):
                w = self.by_cfg("weapon", e._rec["weapons"][slot])
                if w is not None:
                    e.links["weapons"].append(self.link(w, f"slot {slot}"))
            e.links.setdefault("produced_by", [])
            e.links.setdefault("transported_by", [])
            e.links.setdefault("upgrades", [])
            e.links.setdefault("equivalent", [])
        for e in self.by_kind["building"]:
            for k in ("produces", "transports", "upgrade_to", "upgrade_from", "equivalent"):
                e.links.setdefault(k, [])
            e.links["weapon"] = []
        for e in self.by_kind["weapon"]:
            for k in ("used_by_units", "used_by_buildings", "upgrades"):
                e.links.setdefault(k, [])
        for b in self.by_kind["building"]:
            rec = b._rec
            b.btype = self.defines.get(rec.get("type", ""), 0)
            b.brole = b.btype - 20 if b.btype >= 20 else b.btype
            b.role_name = _BROLE_NAME.get(b.brole, "")
        for b in self.by_kind["building"]:
            rec = b._rec
            transports = b.brole in (B_MOTHER, B_PORT, B_SHUTTLE)
            for slot in sorted(rec.get("unit_name", {})):
                u = self.by_cfg("unit", rec["unit_name"][slot])
                if u is None:
                    continue
                b.links["transports" if transports else "produces"].append(
                    self.link(u, f"slot {slot}")
                )
                u.links["transported_by" if transports else "produced_by"].append(self.link(b))
            w = self.by_cfg("weapon", rec.get("weapon", ""))
            if w is not None:
                b.links["weapon"].append(self.link(w))
                w.links["used_by_buildings"].append(self.link(b))
            up = self.by_cfg("building", rec.get("upgrade", ""))
            if up is not None:
                b.links["upgrade_to"].append(self.link(up))
                up.links["upgrade_from"].append(self.link(b))
            eq = self.by_cfg("unit", rec.get("equivalent", ""))
            if eq is not None:
                b.links["equivalent"].append(self.link(eq))
                eq.links["equivalent"].append(self.link(b))
        for u in self.by_kind["unit"]:
            for slot in sorted(u._rec.get("weapons", {})):
                w = self.by_cfg("weapon", u._rec["weapons"][slot])
                if w is not None:
                    w.links["used_by_units"].append(self.link(u))
            eq = self.by_cfg("building", u._rec.get("equivalent", ""))
            if eq is not None and self.link(eq) not in u.links["equivalent"]:
                u.links["equivalent"].append(self.link(eq))
        for up in self.by_kind["upgrade"]:
            rec = up._rec
            up.utype = rec.get("type", "")
            up.links["project"] = [lk for lk in up.links["requires"] if lk.kind == "project"][:1]
            up.links["affects"] = []
            for slot in sorted(rec.get("objects", {})):
                target = self.by_cfg(
                    "weapon" if up.utype == "weapon" else "unit", rec["objects"][slot]
                )
                if target is None:
                    continue
                lk = self.link(target)
                up.objects.append(lk)
                up.links["affects"].append(lk)
                target.links["upgrades"].append(self.link(up))
        for p in self.by_kind["project"]:
            p.links["offers"] = [
                lk for lk in p.links["unlocks"] if lk.kind in ("building", "unit", "upgrade")
            ]
        for s in self.by_kind["system"]:
            s.links["planets"] = []
            for slot in sorted(s._rec.get("planets", {})):
                pl = self.by_cfg("planet", s._rec["planets"][slot])
                if pl is not None:
                    s.links["planets"].append(self.link(pl, f"slot {slot}"))
                    pl.links["system"] = [self.link(s)]
        for pl in self.by_kind["planet"]:
            pl.links.setdefault("system", [])

    def _chains(self):
        """Group buildings by UPGRADE chain and flag the ones nobody can construct."""
        for b in self.by_kind["building"]:
            b.level, b.chain, b.chain_base = 0, [self.link(b)], self.link(b)
            why = UNBUILDABLE_ROLES.get(b.brole)
            b.buildable, b.unbuildable_why = why is None, why or ""
        for base in self.by_kind["building"]:
            if base.links["upgrade_from"]:
                continue
            members, seen = [base], {base.slug}
            while members[-1].links["upgrade_to"]:
                nxt = self.get("building", members[-1].links["upgrade_to"][0].slug)
                if nxt.slug in seen:
                    break
                seen.add(nxt.slug)
                members.append(nxt)
            links = [self.link(m) for m in members]
            for lvl, m in enumerate(members):
                m.level, m.chain, m.chain_base = lvl, links, self.link(base)

    def chain_members(self, b):
        """The buildings of ``b``'s upgrade chain, base first (``[b]`` for a single building)."""
        return [self.get("building", lk.slug) for lk in b.chain]

    def listed(self, kind):
        """Entities that get their own listing: upgrade levels live on their base building's page."""
        if kind == "building":
            return [b for b in self.by_kind["building"] if b.level == 0]
        return list(self.by_kind[kind])

    def starmap_layout(self, system):
        """In-game planet placement of ``system`` on the 1024x768 galaxy screen.

        ``llm_ui_planet_select_screen_build`` (@0x004c5de5) fits the bounding box of the system's
        planets (cfg COORDINATE_X/Y) into (W-128) x (H-210) and adds 32 px: widget x =
        round((cx - min_cx) * (W-128) / (max_cx - min_cx)) + 32, same for y with H-210.  The widget
        is the planet animation (54x54); (x, y) is its top-left corner.
        Returns ``[{"slug", "x", "y"}]`` in slot order.
        """
        pls = [self.get("planet", lk.slug) for lk in system.links.get("planets", [])]
        xs = [p._rec.get("coordinate_x", 0) for p in pls]
        ys = [p._rec.get("coordinate_y", 0) for p in pls]
        if not pls:
            return []

        def fit(vals, span):
            lo, hi = min(vals), max(vals)
            k = span / (hi - lo) if hi > lo else 0.0
            return [rnd((v - lo) * k) + STARMAP_PAD for v in vals]

        px, py = fit(xs, STARMAP_W - 128), fit(ys, STARMAP_H - 210)
        return [{"slug": p.slug, "x": x, "y": y} for p, x, y in zip(pls, px, py, strict=True)]

    # ------------------------------------------------------------------ panels

    def _costs(self, resources, faction, mod_stem=None):
        vals = dict(resources)
        costs = []
        for i, name in enumerate(RES_ORDER):
            v = vals.get(name, 0)
            if mod_stem:
                v = self.mods.apply(mod_stem, v)
            costs.append(Cost(self._key(RES_LABEL[i]), rnd(v)))
        return costs

    def _res_items(self, slotted):
        """extract/capacity/fuel {slot: (res name, float)} -> ore Items (label per ore)."""
        items = []
        for slot in sorted(slotted):
            name, val = slotted[slot]
            if name in RES_ORDER:
                items.append(Item(self._key(RES_LABEL[RES_ORDER.index(name)]), None, rnd(val)))
        return tuple(items)

    def _panels(self):
        for w in self.by_kind["weapon"]:
            self._weapon_panel(w)
        for u in self.by_kind["unit"]:
            self._unit_panel(u)
        for b in self.by_kind["building"]:
            self._building_panel(b)
        for up in self.by_kind["upgrade"]:
            self._upgrade_panel(up)
        for p in self.by_kind["project"]:
            self._project_panel(p)
        for pl in self.by_kind["planet"]:
            self._planet_extra(pl)
        for s in self.by_kind["system"]:
            self._system_extra(s)
        for kind in ("unit", "building", "weapon"):
            for e in self.by_kind[kind]:
                self._art(e)

    def _extra(self, e, rec, spec):
        """Extra rows: cfg fields the game does not show."""
        rows = []
        for cfg_key, fld in spec:
            v = rec.get(fld)
            if _empty(v):
                continue
            rows.append(Row(None, _jsonable(v), None, "", True, NOT_SHOWN, (), cfg_key, False))
        e.extra = rows

    # -- weapons

    def _weapon_power(self, w):
        return rnd(self.mods.apply("WEAPON_POWER", w._rec.get("power", 0)))

    def _weapon_panel(self, w):
        rec = w._rec
        w.target_mask = self.defines.get(rec.get("target", ""), 0)
        p = w.panel
        p.left = [
            self._row("K_MENU_Power", self._weapon_power(w)),
            self._row("K_MENU_RangeMin", rec.get("range_min", 0)),
            self._row("K_MENU_RangeMax", rec.get("range_max", 0)),
        ]
        self._extra(
            w,
            rec,
            [
                ("MISSING", "missing"),
                ("FIRE_RANGE", "fire_range"),
                ("AMMO", "ammo"),
                ("POCKET", "pocket"),
                ("LENGTH", "length"),
                ("SHORT_TIME", "short_time"),
                ("LONG_TIME", "long_time"),
                ("SPEED", "speed"),
                ("TYPE", "type"),
                ("HOMING", "homing"),
                ("TARGET", "target"),
            ],
        )

    def _weapon_row(self, wname, slot=0):
        w = self.by_cfg("weapon", wname)
        if w is None:
            return None
        r = w._rec
        return WeaponRow(
            self.link(w),
            slot,
            self._weapon_power(w),
            r.get("range_min", 0),
            r.get("range_max", 0),
            self.defines.get(r.get("target", ""), 0),
        )

    # -- units

    def _unit_panel(self, u):
        rec = u._rec
        tname = rec.get("type", "")
        m = re.search(r"ZALOGA(\d)", tname)
        u.soldier_count = int(m.group(1)) if m else 0
        verified_k = True
        if "SAMOLOT" in tname:
            cls, k = "plane", SPEED_K["plane"]
        elif tname.endswith("HELIKOPTER"):
            cls, k = "helicopter", SPEED_K["heli"]
        elif tname.endswith("WALKER"):
            cls, k = "walker", SPEED_K["ground"]
        elif tname.endswith("POJAZD"):
            cls, k = "vehicle", SPEED_K["ground"]
        elif m:
            cls, k = "soldier", SPEED_K["other"]
        elif "HELIKOPTERmatka" in tname:
            cls, k, verified_k = "mothership", SPEED_K["other"], False  # UNVERIFIED K
        elif "HELIKOPTERstatek" in tname or "HELIKOPTERprom" in tname:
            cls, k, verified_k = "ship", SPEED_K["other"], False  # UNVERIFIED K
        else:
            cls, k = "other", SPEED_K["other"]
        u.unit_class, u.speed_k = cls, k
        u.hide_production = cls == "mothership"
        p = u.panel
        step = self.mods.apply("UNIT_SPEED", rec.get("step_speed", 0.0))
        if step > 0:
            p.left.append(
                self._row("K_MENU_Speed", rnd(k / step), "K_MENU_Speed_Unit", verified=verified_k)
            )
        p.left.append(
            self._row(
                "K_MENU_Energy",
                rnd(self.mods.apply("UNIT_ENERGY", rec.get("energy", 0))),
                note="HP (hit points) -- not the power resource",
            )
        )
        p.left.append(
            self._row(
                "K_MENU_Sight",
                rnd(self.mods.apply("UNIT_SIGHT", rec.get("sight", 0)) * SIGHT_UI_MUL),
                "K_MENU_Sight_Unit",
            )
        )
        if not u.hide_production:
            # retail bug: build time = bt * MUL + MUL (the ADD slot reads the MUL global)
            bt = rec.get("build_time", 0.0) * self.mods.mul("UNIT_BUILD_TIME") + self.mods.mul(
                "UNIT_BUILD_TIME"
            )
            p.left.append(
                self._row(
                    "K_MENU_ProdTime",
                    rnd(bt),
                    "K_MENU_Time_Unit",
                    note="retail bug: build_time*MUL + MUL",
                )
            )
            p.costs_label_key = self._key("K_MENU_Costs")
            p.costs = self._costs(rec.get("resources", []), u.faction, "UNIT_RESOURCE")
        for slot in sorted(rec.get("weapons", {})):
            wr = self._weapon_row(rec["weapons"][slot], slot)
            if wr:
                p.weapons.append(wr)
        self._extra(
            u,
            rec,
            [
                ("ARMOR", "armor"),
                ("P_1", "p_1"),
                ("P_2", "p_2"),
                ("P_3", "p_3"),
                ("P_4", "p_4"),
                ("P_5", "p_5"),
                ("HUMAN", "human"),
                ("TRACE", "trace"),
                ("AILEVEL", "ai_level"),
                ("TURN_SPEED", "turn_speed"),
                ("AIUNIT", "aiunit"),
                ("SOLDIER_TYPE", "soldier_type"),
                ("INDEPENDENT", "independent"),
                ("TYPE", "type"),
            ],
        )

    # -- buildings

    def _building_panel(self, b):
        rec = b._rec
        role = b.brole
        fac = b.faction
        p = b.panel
        ability = rec.get("ability", 0)
        # retail bug: power = (electric_power + BUILDING_ENERGY_ADD) * BUILDING_POWER_MUL
        power = (rec.get("electric_power", 0) + self.mods.add("BUILDING_ENERGY")) * self.mods.mul(
            "BUILDING_POWER"
        )
        p.left.append(
            self._row(
                "K_MENU_EnergyProd" if role in (B_PLANT, B_MOTHER) else "K_MENU_EnergyReq",
                rnd(power),
                note="retail bug: (electric_power + BUILDING_ENERGY_ADD) * BUILDING_POWER_MUL",
            )
        )
        p.left.append(
            self._row(
                "K_MENU_Energy",
                rnd(self.mods.apply("BUILDING_ENERGY", rec.get("energy", 0))),
                note="HP (hit points) -- not the power resource",
            )
        )
        human = rnd(self.mods.apply("BUILDING_HUMAN", rec.get("human", 0)))
        if human > 0:
            if role in (B_COLONY, B_MOTHER):
                p.left.append(self._row(self._lk("K_MENU_MaxPeople", fac), human))
            else:
                p.left.append(
                    self._row(
                        "K_MENU_MaxWorkers",
                        human,
                        verified=False,
                        note="UNVERIFIED: HUMAN (BUILDER is the alternative)",
                    )
                )
        if role != B_MOTHER:
            p.left.append(
                self._row(
                    "K_MENU_BuildTime",
                    rnd(self.mods.apply("BUILDING_BUILD_TIME", rec.get("build_time", 0.0))),
                    "K_MENU_Time_Unit",
                    verified=False,
                    note="UNVERIFIED: uses the modified build time",
                )
            )
        vel = rec.get("velocity", 0.0)
        if vel > 0 and (role in (B_MOTHER, B_PORT) or (role == B_SHUTTLE and ability)):
            p.left.append(
                self._row(
                    "K_MENU_FlightSpeed",
                    rnd(1000.0 / vel),
                    "K_MENU_FlightSpeed_Unit",
                    verified=role != B_SHUTTLE,
                )
            )
        if role != B_MOTHER:
            p.costs_label_key = self._key("K_MENU_Costs")
            p.costs = self._costs(rec.get("resources", []), fac)
        p.right = self._building_right(b)
        for wname in [rec.get("weapon", "")]:
            wr = self._weapon_row(wname) if wname else None
            if wr:
                p.weapons.append(wr)
        self._extra(
            b,
            rec,
            [
                ("SIGHT", "sight"),
                ("BUILDER", "builder"),
                ("TRACE", "trace"),
                ("ABILITY", "ability"),
                ("HEIGHT", "height"),
                ("COMPONENT_QUANT", "component_quant"),
                ("SPRITE_QUANTITY", "sprite_quantity"),
                ("PARAMETR", "housing_capacity"),
                ("TYPE", "type"),
                ("AIBLD", "aibld"),
                ("ANIM_P", "anim_p"),
                ("AREA", "area"),
            ],
        )
        # upgrade block: filled once every building has its right column

    def _building_right(self, b):
        rec = b._rec
        role = b.brole
        rows = []
        if role == B_FACTORY:
            items, trains = [], False
            for slot in sorted(rec.get("unit_name", {})):
                u = self.by_cfg("unit", rec["unit_name"][slot])
                if u is None:
                    continue
                trains = (
                    trains
                    or u.soldier_count > 0
                    or u._rec.get("soldier_type", "") not in ("", "SOLDIERTYPE_UNKNOWN")
                )
                items.append(Item(None, self.link(u), rec.get("unit_quant", {}).get(slot)))
            rows.append(
                self._row(
                    "K_MENU_UnitsTrained" if trains else "K_MENU_UnitsProduced",
                    None,
                    items=tuple(items),
                )
            )
        elif role == B_TURRET:
            w = self.by_cfg("weapon", rec.get("weapon", ""))
            if w is not None:
                rows.append(self._row("K_MENU_StrikePower", self._weapon_power(w)))
        elif role == B_MINE:
            rows.append(
                self._row(
                    "K_MENU_ResMined",
                    None,
                    verified=False,
                    items=self._res_items(rec.get("extract_fuel", {})),
                    note="UNVERIFIED: float -> int rounding",
                )
            )
        elif role == B_STORAGE:
            rows.append(
                self._row(
                    "K_MENU_ResStored",
                    None,
                    verified=False,
                    items=self._res_items(rec.get("capacity", {})),
                    note="UNVERIFIED: float -> int rounding",
                )
            )
        elif role in B_STORE_UNITS:
            rows.append(
                self._row("K_MENU_UnitsStored", STORAGE_UNITS, note="hardcoded in the game")
            )
        elif role in (B_MOTHER, B_PORT):
            rows.append(
                self._row(
                    "K_MENU_FlightCosts",
                    None,
                    verified=False,
                    items=self._res_items(rec.get("extract_fuel", {})),
                    note="UNVERIFIED: float -> int rounding",
                )
            )
        elif role == B_SHUTTLE:
            if rec.get("ability", 0):
                for i in (1, 2, 3):
                    rows.append(
                        self._row(
                            f"K_MENU_Possibility{i}",
                            None,
                            verified=False,
                            note="UNVERIFIED: shown when ABILITY is set",
                        )
                    )
            rows.append(
                self._row(
                    "K_MENU_FlightCosts",
                    None,
                    verified=False,
                    items=self._res_items(rec.get("extract_fuel", {})),
                    note="UNVERIFIED: float -> int rounding",
                )
            )
        return rows

    # -- upgrades / projects

    def _upgrade_panel(self, up):
        rec = up._rec
        p = up.panel

        def signed(label, v, key=None):
            if v:
                p.left.append(
                    self._row(
                        label,
                        abs(rnd(v)),
                        None,
                        "+" if v > 0 else "-",
                        verified=False,
                        note="UNVERIFIED semantics of the upgrade delta",
                        cfg_key=key or "",
                    )
                )

        if up.utype == "weapon":
            signed("K_MENU_RangeMin", rec.get("range_min", 0), "RANGE_MIN")
            signed("K_MENU_RangeMax", rec.get("range_max", 0), "RANGE_MAX")
            signed("K_MENU_Inaccur", rec.get("missing", 0), "MISSING")
            signed("K_MENU_Power", rec.get("power", 0), "POWER")
        else:
            signed("K_MENU_Speed", rec.get("step_speed", 0.0), "STEP_SPEED")
            signed("K_MENU_Armor", rec.get("energy", 0), "ENERGY")
        self._extra(up, rec, [("SIGHT", "sight"), ("TURN_SPEED", "turn_speed")])

    def _building_upgrade_blocks(self):
        for b in self.by_kind["building"]:
            tgt = b.links["upgrade_to"][:1]
            if not tgt:
                continue
            t = self.get("building", tgt[0].slug)
            proj = [lk for lk in t.links["research"]][:1]
            b.panel.upgrade = {
                "target": tgt[0],
                "project": proj[0] if proj else None,
                "panel": t.panel,
            }

    def _project_panel(self, pr):
        rec = pr._rec
        p = pr.panel
        p.costs_label_key = self._key("K_MENU_Costs")
        p.costs = self._costs(rec.get("resources", []), pr.faction)
        p.research = self._row(
            "K_MENU_BuildTime",
            rnd(rec.get("build_time", 0.0)),
            "K_MENU_Time_Unit",
            note="wiki addition: research time, not shown in the game",
            shown=False,
        )
        for lk in pr.links["offers"]:
            e = self.get(lk.kind, lk.slug)
            if e.kind == "building":
                upgraded = bool(e.links["upgrade_from"])
                p.offers.append(
                    Offer(
                        "building_upgrade" if upgraded else "building_new",
                        self._key("K_MENU_BuildUpg" if upgraded else "K_MENU_BuildNew"),
                        lk,
                        e.panel,
                    )
                )
            elif e.kind == "unit":
                p.offers.append(Offer("unit", None, lk, e.panel))
            else:
                affected = []
                for a in e.links["affects"]:
                    if e.utype == "unit":
                        tu = self.get("unit", a.slug)
                        if tu.soldier_count >= 2:
                            continue
                    affected.append(a)
                p.offers.append(
                    Offer(
                        "upgrade_weapon" if e.utype == "weapon" else "upgrade_unit",
                        None,
                        lk,
                        e.panel,
                        affected,
                    )
                )
        # description fallback: empty INFO_TXT -> first unlocked building/unit's text
        for lang in self.langs:
            if pr.description.get(lang):
                continue
            for off in p.offers:
                if off.role in ("unit", "building_new", "building_upgrade"):
                    src = self.get(off.link.kind, off.link.slug)
                    if src.description.get(lang):
                        pr.description[lang] = src.description[lang]
                        break
        if not pr.icon:
            for off in p.offers:  # projects: icon of the first unlocked thing (or affected unit)
                src = self.get(off.link.kind, off.link.slug)
                cands = [src] + [self.get(a.kind, a.slug) for a in off.affected]
                hit = next((c for c in cands if c.icon), None)
                if hit is not None:
                    pr.icon, pr.icon_slot = hit.icon, hit.icon_slot
                    break
        pr.art = {"info_flc": rec.get("info_flc", ""), "icon": pr.icon}

    # -- planets / systems / art

    def _planet_extra(self, pl):
        rec = pl._rec
        pl.art = {"info_flc": rec.get("info_flc", ""), "icon": pl.icon, "map": rec.get("map", "")}
        self._extra(
            pl,
            rec,
            [
                ("MAP", "map"),
                ("COORDINATE_X", "coordinate_x"),
                ("COORDINATE_Y", "coordinate_y"),
                ("ASTEROIDS", "asteroids"),
                ("ENEMY", "enemy"),
                ("TURN_SPEED", "turn_speed"),
                ("X1", "x1"),
                ("Y1", "y1"),
                ("X2", "x2"),
                ("Y2", "y2"),
                ("SOURCE_MUL", "source_mul"),
                ("SOURCE_ADD", "source_add"),
                ("BANK", "banks"),
            ],
        )
        banks = rec.get("banks", set())
        # the 50..56 bank is the planet's tree bank; its biome name is UNVERIFIED so it stays a number
        trees = sorted(b for b in banks if 50 <= b <= 56)
        pl.art["tree_bank"] = trees[0] if trees else None

    def _system_extra(self, s):
        s.art = {"icon": s.icon}

    def _art(self, e):
        rec = e._rec
        keys = {
            "unit": (
                "sprite",
                "sprite_shadow",
                "sprite_type",
                "frame",
                "frame_2",
                "anim_explo",
                "info_flc",
            ),
            "building": ("frame", "frame_2", "anims", "components", "info_flc", "anim_explo"),
            "weapon": ("fire_explo", "target_explo", "smoke_sprite", "bullet_anims"),
        }[e.kind]
        e.art = {k: _jsonable(rec.get(k)) for k in keys if not _empty(rec.get(k))}
        e.art["icon"] = e.icon

    # -- weapon factions and bookkeeping

    def _weapon_factions(self):
        self._building_upgrade_blocks()
        for w in self.by_kind["weapon"]:
            facs = set()
            for lk in w.links["used_by_units"] + w.links["used_by_buildings"]:
                facs.add(self.get(lk.kind, lk.slug).faction)
            facs.discard(None)
            w.faction = facs.pop() if len(facs) == 1 else None

    def _finish_missing(self):
        self.missing = {}
        en = self.texts.get("en", {})
        used = self._used | self._name_keys
        for lang in self.langs:
            t = self.texts[lang]
            self.missing[lang] = sorted(k for k in used if k not in t)
        self.missing_everywhere = sorted(k for k in used if k not in en)

    # ------------------------------------------------------------------ tech tree

    def tech_graph(self, faction=None):
        """Invention graph for the tech-tree writer (see the module docstring)."""
        keep = None
        if faction is not None:
            keep = {n for n, inv in self.inventions.items() if inv.faction == faction}
            todo = list(keep)
            while todo:  # neutral prerequisites (planets, systems), transitively
                n = todo.pop()
                for d in self.inventions[n].depends:
                    if (
                        d in self.inventions
                        and d not in keep
                        and self.inventions[d].faction is None
                    ):
                        keep.add(d)
                        todo.append(d)
        nodes, edges = [], []
        for name, inv in self.inventions.items():
            if keep is not None and name not in keep:
                continue
            owner = self._inv_owner.get(name)
            nodes.append(
                {
                    "id": name,
                    "type": inv.type,
                    "faction": inv.faction,
                    "owner": inv.owner,
                    "page": inv.page,
                    "icon": owner.icon if owner is not None else "",
                    "icon_slot": owner.icon_slot if owner is not None else None,
                    "names": dict(owner.names) if owner is not None else {},
                    "depends": [d for d in inv.depends if keep is None or d in keep],
                }
            )
            for d in inv.depends:
                if d in self.inventions and (keep is None or d in keep):
                    edges.append((d, name))
        return {"nodes": nodes, "edges": edges}

    # ------------------------------------------------------------------ debugging

    def summary(self):
        return {
            "counts": self.counts(),
            "inventions": len(self.inventions),
            "missing": {k: len(v) for k, v in self.missing.items()},
        }


def build(data) -> Wiki:
    """GameData (``source.load`` / ``source.load_files``) -> :class:`Wiki`."""
    return Wiki(data)


# --------------------------------------------------------------------------------- acceptance


def _find(wiki, kind, cfg_name):
    return wiki.by_cfg(kind, cfg_name)


def _row(entity, label_key, side="left"):
    for r in getattr(entity.panel, side):
        if r.label_key in (label_key, label_key + "_a"):
            return r
    return None


def main(argv=None):  # pragma: no cover -- retail acceptance printout
    import argparse

    ap = argparse.ArgumentParser(
        description="Build the wiki model from retail data and print the acceptance numbers."
    )
    ap.add_argument(
        "--game", default=None, help="EN install (dir or zip); default via machine_config"
    )
    ap.add_argument(
        "--ru", default=None, help="RU install/pack layer (dir or zip); default via machine_config"
    )
    a = ap.parse_args(argv)
    game = a.game or _source.default_game()
    ru = a.ru or _source.default_ru()
    if not game:
        raise SystemExit("no EN install found; pass --game")
    for label, ru_arg in (("en", None), ("en+ru", ru)):
        wiki = build(_source.load(game, ru_arg))
        print(label, wiki.langs, wiki.counts(), "inventions", len(wiki.inventions))
        print(" missing:", {k: v for k, v in wiki.missing.items() if v})
    s3 = _find(wiki, "unit", "N_human_Soldier3")
    print(
        "Soldier3 sight",
        _row(s3, "K_MENU_Sight").value,
        "m  energy",
        _row(s3, "K_MENU_Energy").value,
    )
    re_ = _find(wiki, "building", "N_alien_Elektrownia")
    print(
        "alien Reactor power",
        _row(re_, "K_MENU_EnergyProd").value,
        "energy",
        _row(re_, "K_MENU_Energy").value,
    )


if __name__ == "__main__":  # pragma: no cover
    main()
