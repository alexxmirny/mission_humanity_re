"""Static wiki site: one tree per language, built from the W1 view-model and the W2 art maps.

Public API::

    from src.formats.wiki import source, model, site
    wiki = model.build(source.load(en_install, ru=ru_install))
    art_maps = site.load_art_maps(art_dir)          # art_dir/assets.json written by the art CLI
    report = site.build_site(wiki, art_maps, out_dir, langs=("en", "ru"), art_dir=art_dir)
    problems = site.run_checks(wiki, report.mapping, out_dir, langs)   # {check name: [problem]}

``art_maps`` is ``{"icons": {...}, "buildings": {...}, "units": {...}, "planets": {...},
"stills": {...}}`` -- the ``extract_*`` return values of ``src.formats.wiki.art`` (paths relative to
``art_dir``).  ``build_site`` copies ``art_dir/{icons,sprites,planets,stills}`` to ``out_dir/assets``.

Layout::

    out/index.html                       language picker
    out/assets/{site.css,site.js,techtree.*,icons,sprites,planets,stills}
    out/<lang>/index.html                home
    out/<lang>/{units,buildings,projects,weapons,planets,systems}/{index,<slug>}.html
    out/<lang>/techtree/{index,alien,human}.html
    out/<lang>/search.json  search.js    (search.js is the same data for file:// use)

The three key spaces (cfg record -> W1 entity slug -> W2 art key -> tech-tree node id) are
reconciled in :func:`build_mapping`, the only place that knows how they relate.  Every mismatch is
recorded on the ``Mapping`` (errors = inconsistent key spaces, warnings = art the cfg names but the
art pass could not produce) and ``build_site`` raises :class:`SiteBuildError` on errors when strict.

ENERGY is the HP-like stat and POWER the generated resource; the game's "Energy:" row is labelled
"Energy (HP):" here.
"""

from __future__ import annotations

import json
import re
import shutil
import time
from collections import Counter
from dataclasses import dataclass, field
from html.parser import HTMLParser
from pathlib import Path

import jinja2
from markupsafe import Markup

from . import techtree
from .model import STARMAP_ANIM, STARMAP_W

HERE = Path(__file__).resolve().parent

PAGE_KINDS = ("unit", "building", "project", "weapon", "planet", "system")
KIND_DIR = {
    "unit": "units",
    "building": "buildings",
    "project": "projects",
    "weapon": "weapons",
    "planet": "planets",
    "system": "systems",
}
LANG_NAME = {"en": "English", "ru": "Русский"}
ASSET_DIRS = ("icons", "sprites", "planets", "stills")

# link roles in display order
ROLE_ORDER = (
    "requires",
    "unlocks",
    "research",
    "produced_by",
    "produces",
    "transported_by",
    "transports",
    "weapons",
    "weapon",
    "upgrade_to",
    "upgrade_from",
    "upgrades",
    "equivalent",
    "used_by_units",
    "used_by_buildings",
    "system",
    "planets",
    "affects",
    "offers",
)

# ------------------------------------------------------------------------------ site chrome
# Game text (stat labels, ore names, units) always comes from the game's TEXT keys through
# Wiki.label; only the strings the game has no key for live here.

_EN = {
    "site": "Mission Humanity game wiki",
    "lead": "Every unit, building, research project, weapon, planet and star system, with the "
    "numbers the in-game info panel shows. Generated from the game's data files.",
    "home": "Home",
    "search": "Search...",
    "no_results": "Nothing found",
    "name": "Name",
    "entries": "entries",
    "sort_hint": "Click a column header to sort.",
    "alien": "Alien",
    "human": "Human",
    "shared": "Shared / other",
    "all": "All",
    "techtree": "Tech tree",
    "tt_lead": "Research dependencies: an invention unlocks only when all of its prerequisites are acquired.",
    "tt_faction": "Tech tree: {}",
    "tt_squads": "Squads of 2-5 soldiers (or insects) are not in the tree: they cannot be built directly, they form by merging single soldiers on the field. Their pages stay in the wiki.",
    "about": "About",
    "about_text": "Values are the ones the game's info panel computes (cfg values after the global "
    "modifiers, rounded half up). Rows marked ? follow the best reading of the game code and are "
    "not confirmed. Fields the game does not show are listed under 'Config values not shown in game'.",
    "missing_note": "Texts missing in this language (English shown instead):",
    "footer": "Generated from the game's data files. Fan-made reference.",
    "panel": "In-game info panel",
    "description": "Description",
    "desc_fallback": "No text in this language; English shown.",
    "related": "Related",
    "extra": "Config values not shown in game",
    "unverified": "Unverified",
    "unverified_mark": "?",
    "cfg_name": "Config name",
    "weapon": "Weapon",
    "range": "Range",
    "targets": "Targets",
    "ground": "ground",
    "air": "air",
    "upgrade_possible": "Upgrade possible:",
    "level_base": "Level 1 (base building)",
    "level_up": "Upgrade {}",
    "levels": "Levels",
    "levels_n": "{} levels",
    "upgrade_of": "This is an upgrade of a building; it is shown on the page of the base building.",
    "upgrade_moved": "Open the page of the base building:",
    "not_buildable": "Cannot be built in a normal game",
    "starmap": "Star map",
    "planet_map": "Map",
    "planet_map_size": "Map size",
    "planet_map_tiles": "{w} x {h} tiles",
    "planet_map_note": "Terrain preview of the planet's map (retail .MP file), one tile = a few pixels.",
    "map_landing": "Landing spot",
    "map_ore": "Ore deposit",
    "starmap_note": "Planets at their in-game positions on the galaxy screen. Click a planet.",
    "research_required": "Research required",
    "research_time": "Research time:",
    "not_in_game": "not shown in game",
    "affects": "Affects",
    "cap_icon": "Icon",
    "cap_sprite": "Completed building",
    "cap_unit": "Unit",
    "cap_still": "Video frame",
    "cap_planet": "Planet",
    "kinds": {
        "unit": "Unit",
        "building": "Building",
        "project": "Research project",
        "weapon": "Weapon",
        "planet": "Planet",
        "system": "Star system",
        "upgrade": "Upgrade",
    },
    "kinds_pl": {
        "unit": "Units",
        "building": "Buildings",
        "project": "Research projects",
        "weapon": "Weapons",
        "planet": "Planets",
        "system": "Star systems",
    },
    "offer": {
        "building_new": "New building",
        "building_upgrade": "Building upgraded",
        "unit": "New unit",
        "upgrade_unit": "Unit upgrade",
        "upgrade_weapon": "Weapon upgrade",
    },
    "roles": {
        "requires": "Requires",
        "unlocks": "Unlocks",
        "research": "Researched by project",
        "produced_by": "Produced by",
        "produces": "Produces",
        "transported_by": "Transported by",
        "transports": "Transports",
        "weapons": "Weapons",
        "weapon": "Weapon",
        "upgrade_to": "Upgrades to",
        "upgrade_from": "Upgrade of",
        "upgrades": "Upgraded by",
        "equivalent": "Equivalent of the other side",
        "used_by_units": "Used by units",
        "used_by_buildings": "Used by buildings",
        "system": "Star system",
        "planets": "Planets",
        "affects": "Affects",
        "offers": "Offers",
    },
    "building_role": {
        "factory": "Factory",
        "mine": "Mine",
        "plant": "Power plant",
        "colony": "Colony",
        "turret": "Turret",
        "mother": "Mothership",
        "garage": "Garage",
        "barracks": "Barracks",
        "hangar": "Hangar",
        "helipad": "Helipad",
        "laboratory": "Laboratory",
        "port": "Spaceport",
        "shuttle": "Shuttle",
        "base": "Base",
        "relay": "Relay",
        "storage": "Storage",
        "office": "Office",
        "civil": "Civil building",
    },
    "unit_class": {
        "soldier": "Soldier",
        "vehicle": "Vehicle",
        "walker": "Walker",
        "plane": "Aircraft",
        "helicopter": "Helicopter",
        "mothership": "Mothership",
        "ship": "Ship",
        "other": "Other",
    },
    "col_type": "Type",
    "col_weapons": "Weapons",
    "col_power": "Power",
    "col_system": "Star system",
    "col_unlocks": "Unlocks",
    "col_users": "Used by",
    "col_planets": "Planets",
    "hp": "HP",
    "tt_ui": {},
}

_RU = {
    "site": "Вики по игре Mission Humanity",
    "lead": "Все юниты, здания, проекты исследований, оружие, планеты и звёздные системы с "
    "цифрами, которые показывает игровая панель информации. Построено по файлам данных игры.",
    "home": "Главная",
    "search": "Поиск...",
    "no_results": "Ничего не найдено",
    "name": "Название",
    "entries": "записей",
    "sort_hint": "Нажмите на заголовок столбца для сортировки.",
    "alien": "Пришельцы",
    "human": "Люди",
    "shared": "Общее / прочее",
    "all": "Все",
    "techtree": "Дерево технологий",
    "tt_lead": "Зависимости исследований: изобретение открывается, только когда получены все его предпосылки.",
    "tt_faction": "Дерево технологий: {}",
    "tt_squads": "Отряды из 2-5 солдат (или насекомых) в дереве не показаны: напрямую их построить нельзя, они образуются слиянием одиночных бойцов на поле. Их страницы остались в вики.",
    "about": "О справочнике",
    "about_text": "Значения такие, какие считает игровая панель информации (значения из конфигурации "
    "после глобальных модификаторов, округление к большему). Строки со знаком ? соответствуют "
    "лучшему прочтению кода игры и не подтверждены. Поля, которые игра не показывает, собраны в "
    "разделе «Параметры конфигурации, которых нет в игре».",
    "missing_note": "Тексты, отсутствующие в этом языке (показаны английские):",
    "footer": "Построено по файлам данных игры. Справочник энтузиастов.",
    "panel": "Панель информации в игре",
    "description": "Описание",
    "desc_fallback": "Нет текста на этом языке; показан английский.",
    "related": "Связанное",
    "extra": "Параметры конфигурации, которых нет в игре",
    "unverified": "Не подтверждено",
    "unverified_mark": "?",
    "cfg_name": "Имя в конфигурации",
    "weapon": "Оружие",
    "range": "Дальность",
    "targets": "Цели",
    "ground": "наземные",
    "air": "воздушные",
    "upgrade_possible": "Возможно улучшение:",
    "level_base": "Уровень 1 (базовое здание)",
    "level_up": "Улучшение {}",
    "levels": "Уровни",
    "levels_n": "Уровней: {}",
    "upgrade_of": "Это улучшение здания; оно показано на странице базового здания.",
    "upgrade_moved": "Открыть страницу базового здания:",
    "not_buildable": "В обычной игре построить нельзя",
    "starmap": "Звёздная карта",
    "planet_map": "Карта",
    "planet_map_size": "Размер карты",
    "planet_map_tiles": "{w} x {h} клеток",
    "planet_map_note": "Предпросмотр рельефа карты планеты (файл .MP из игры), одна клетка - несколько пикселей.",
    "map_landing": "Место посадки",
    "map_ore": "Залежи руды",
    "starmap_note": "Планеты на своих игровых местах на экране галактики. Нажмите на планету.",
    "research_required": "Требуется исследование",
    "research_time": "Время исследования:",
    "not_in_game": "в игре не показывается",
    "affects": "Затрагивает",
    "cap_icon": "Значок",
    "cap_sprite": "Построенное здание",
    "cap_unit": "Юнит",
    "cap_still": "Кадр из видео",
    "cap_planet": "Планета",
    "kinds": {
        "unit": "Юнит",
        "building": "Здание",
        "project": "Проект исследования",
        "weapon": "Оружие",
        "planet": "Планета",
        "system": "Звёздная система",
        "upgrade": "Улучшение",
    },
    "kinds_pl": {
        "unit": "Юниты",
        "building": "Здания",
        "project": "Проекты исследований",
        "weapon": "Оружие",
        "planet": "Планеты",
        "system": "Звёздные системы",
    },
    "offer": {
        "building_new": "Новое здание",
        "building_upgrade": "Здание улучшено",
        "unit": "Новый юнит",
        "upgrade_unit": "Улучшение юнита",
        "upgrade_weapon": "Улучшение оружия",
    },
    "roles": {
        "requires": "Требует",
        "unlocks": "Открывает",
        "research": "Исследуется проектом",
        "produced_by": "Производится в",
        "produces": "Производит",
        "transported_by": "Перевозится в",
        "transports": "Перевозит",
        "weapons": "Оружие",
        "weapon": "Оружие",
        "upgrade_to": "Улучшается до",
        "upgrade_from": "Улучшение для",
        "upgrades": "Улучшается проектом",
        "equivalent": "Аналог другой стороны",
        "used_by_units": "Используется юнитами",
        "used_by_buildings": "Используется зданиями",
        "system": "Звёздная система",
        "planets": "Планеты",
        "affects": "Затрагивает",
        "offers": "Предлагает",
    },
    "building_role": {
        "factory": "Фабрика",
        "mine": "Шахта",
        "plant": "Электростанция",
        "colony": "Колония",
        "turret": "Турель",
        "mother": "Корабль-матка",
        "garage": "Гараж",
        "barracks": "Казарма",
        "hangar": "Ангар",
        "helipad": "Вертолётная площадка",
        "laboratory": "Лаборатория",
        "port": "Космопорт",
        "shuttle": "Шаттл",
        "base": "База",
        "relay": "Ретранслятор",
        "storage": "Склад",
        "office": "Офис",
        "civil": "Гражданское здание",
    },
    "unit_class": {
        "soldier": "Пехота",
        "vehicle": "Техника",
        "walker": "Шагоход",
        "plane": "Самолёт",
        "helicopter": "Вертолёт",
        "mothership": "Корабль-матка",
        "ship": "Корабль",
        "other": "Прочее",
    },
    "col_type": "Тип",
    "col_weapons": "Оружие",
    "col_power": "Энергия (ресурс)",
    "col_system": "Звёздная система",
    "col_unlocks": "Открывает",
    "col_users": "Используется",
    "col_planets": "Планеты",
    "hp": "HP",
    "tt_ui": {
        "unit": "Юнит",
        "building": "Здание",
        "project": "Проект",
        "upgrade": "Улучшение",
        "planet": "Ворота: планета",
        "system": "Ворота: система",
        "legend": "Условные обозначения",
        "rule": "Узел открывается, только когда получены ВСЕ его предпосылки (слева направо).",
        "help": "Нажмите на узел, чтобы подсветить цепочку предпосылок и то, что он открывает. "
        "Нажмите на пустое место, чтобы снять выделение.",
        "requires": "Требует (всё из)",
        "unlocks": "Открывает",
        "open": "Открыть страницу",
        "start": "Изобретение с начала игры",
        "fit": "Вписать",
        "gate": "пунктир = ворота планеты / системы",
        "derived": "точки = правило игры, в конфиге его нет",
        "none": "ничего",
        "n_nodes": "изобретений",
        "n_links": "связей",
        "n_ranks": "уровней",
    },
}

CHROME = {"en": _EN, "ru": _RU}


def chrome(lang):
    """Site-chrome strings for ``lang`` (EN fills any key the language lacks)."""
    out = dict(_EN)
    out.update(CHROME.get(lang, {}))
    return out


class SiteBuildError(RuntimeError):
    """Raised by :func:`build_site` (strict) when the key spaces do not reconcile."""

    def __init__(self, errors):
        self.errors = list(errors)
        super().__init__(f"{len(self.errors)} site build errors; first: {self.errors[:5]}")


# ------------------------------------------------------------------------------ key mapping


def still_key(ref):
    """W2's still key: video ref lower-cased with '/' separators."""
    return ref.lower().replace("\\", "/")


def occurrence_keys(entities):
    """W2 ``object_keys`` rule for W1 entities: cfg name, or ``name#k`` when the name repeats."""
    counts = Counter(e.cfg_name for e in entities)
    seen = Counter()
    keys = []
    for e in entities:
        k = seen[e.cfg_name]
        seen[e.cfg_name] += 1
        keys.append(f"{e.cfg_name}#{k}" if counts[e.cfg_name] > 1 else e.cfg_name)
    return keys


@dataclass
class Ref:
    """Resolved asset paths (relative to ``assets/``) of one entity; '' = none."""

    art_key: str = ""
    icon: str = ""
    sprite: str = ""
    still: str = ""
    planet: str = ""
    starmap: str = ""
    mapimg: str = ""  # planet map preview (JPEG, terrain only)
    mapthumb: str = ""
    mapinfo: dict = field(default_factory=dict)  # {"w","h","map","land": [[x, y]], "ore": [[x, y]]}


@dataclass
class Mapping:
    refs: dict = field(default_factory=dict)  # (kind, slug) -> Ref
    inv_page: dict = field(default_factory=dict)  # invention -> (kind, slug) of its page, or None
    inv_owner: dict = field(default_factory=dict)  # invention -> (kind, slug) of its owner
    errors: list = field(default_factory=list)
    warnings: list = field(default_factory=list)
    stats: dict = field(default_factory=dict)

    def ref(self, e):
        return self.refs.get((e.kind, e.slug)) or Ref()


# A video still shared by more than this many entities says nothing about any of them (the
# same clip is reused for dozens of projects / units), so it is not shown.  Projects never show
# one (user review 2026-10-26: "it is the same for all").
STILL_SHARED_MAX = 3
STILL_KINDS = ("unit", "building")


def _video_ref(e):
    v = e.art.get("info_flc") or e.raw.get("info_flc") or ""
    if not isinstance(v, str) or not v.lower().endswith(".avi"):
        return ""
    key = still_key(v)
    return "" if key.rsplit("/", 1)[-1] == "not.avi" else key  # NOT.AVI = "no video" placeholder


def build_mapping(wiki, art_maps):
    """Reconcile cfg record -> entity slug -> art key -> invention node id.

    Errors: art keys nobody claims (orphans), duplicated-name groups whose art keys do not line
    up, inventions the tech graph and the model disagree about, links to missing entities.
    Warnings: art the cfg names but the art maps lack.
    """
    m = Mapping()
    icons = art_maps.get("icons", {})
    maps = {
        "building": art_maps.get("buildings", {}),
        "unit": art_maps.get("units", {}),
        "planet": art_maps.get("planets", {}),
    }
    stills = art_maps.get("stills", {})
    starmaps = art_maps.get("starmaps", {})
    pmaps = art_maps.get("planetmaps", {})
    pthumbs = art_maps.get("planetthumbs", {})
    pinfo = art_maps.get("planetmapinfo", {})
    claimed = {
        "icons": set(),
        "building": set(),
        "unit": set(),
        "planet": set(),
        "stills": set(),
        "starmaps": set(),
        "planetmaps": set(),
    }
    vcount = Counter(
        _video_ref(e)
        for kind in ("unit", "building", "project")
        for e in wiki.by_kind[kind]
        if _video_ref(e) and not (kind == "building" and e.level > 0)
    )
    missing = {"icon": [], "sprite": [], "still": [], "planet": []}
    seen_slugs = set()

    for kind in ("unit", "building", "project", "weapon", "planet", "system", "upgrade"):
        ents = wiki.by_kind[kind]
        keys = occurrence_keys(ents)
        for e, key in zip(ents, keys, strict=True):
            if (kind, e.slug) in seen_slugs:
                m.errors.append(f"duplicate slug {kind}/{e.slug}")
            seen_slugs.add((kind, e.slug))
            r = Ref(art_key=key)
            if e.icon:
                claimed["icons"].add(e.icon)
                if e.icon in icons:
                    r.icon = icons[e.icon]
                else:
                    missing["icon"].append(f"{kind}/{e.slug}:{e.icon}")
            if kind in ("unit", "building"):
                claimed[kind].add(key)
                if key in maps[kind]:
                    r.sprite = maps[kind][key]
                else:
                    missing["sprite"].append(f"{kind}/{e.slug}:{key}")
            if kind == "planet":
                claimed["planet"].add(key)
                if key in maps["planet"]:
                    r.planet = maps["planet"][key]
                else:
                    missing["planet"].append(f"{kind}/{e.slug}:{key}")
                if key in pmaps:  # planet maps are optional (no retail data -> none)
                    claimed["planetmaps"].add(key)
                    r.mapimg, r.mapthumb = pmaps[key], pthumbs.get(key, "")
                    r.mapinfo = pinfo.get(key, {})
            if kind == "system" and e.cfg_name in starmaps:
                claimed["starmaps"].add(e.cfg_name)
                r.starmap = starmaps[e.cfg_name]
            if kind in ("unit", "building", "project"):
                vk = _video_ref(e)
                if vk:
                    claimed["stills"].add(vk)
                    shown = (
                        kind in STILL_KINDS
                        and vcount[vk] <= STILL_SHARED_MAX
                        and not (kind == "building" and e.level > 0)
                    )
                    if not shown:
                        pass
                    elif vk in stills:
                        r.still = stills[vk]
                    elif stills:  # stills are optional: only complain when some exist
                        missing["still"].append(f"{kind}/{e.slug}:{vk}")
            m.refs[(kind, e.slug)] = r

    for what, mp in (
        ("icon", icons),
        ("building sprite", maps["building"]),
        ("unit sprite", maps["unit"]),
        ("planet frame", maps["planet"]),
        ("still", stills),
        ("starmap", starmaps),
        ("planet map", pmaps),
    ):
        ck = {
            "icon": "icons",
            "building sprite": "building",
            "unit sprite": "unit",
            "planet frame": "planet",
            "still": "stills",
            "starmap": "starmaps",
            "planet map": "planetmaps",
        }[what]
        for k in sorted(set(mp) - claimed[ck]):
            m.errors.append(f"orphan {what} key {k!r}: no entity claims it")
    for what, lst in missing.items():
        if lst:
            m.warnings.append(f"{len(lst)} entities name a {what} the art pass did not produce")
            m.stats[f"missing_{what}"] = lst

    # invention node ids <-> entities <-> tech-graph nodes
    for name, inv in wiki.inventions.items():
        for slot, link in (("inv_page", inv.page), ("inv_owner", inv.owner)):
            if link is None:
                getattr(m, slot)[name] = None
                continue
            try:
                wiki.get(link.kind, link.slug)
            except KeyError:
                m.errors.append(
                    f"invention {name}: {slot[4:]} {link.kind}/{link.slug} not in model"
                )
                getattr(m, slot)[name] = None
                continue
            getattr(m, slot)[name] = (link.kind, link.slug)
        if m.inv_page[name] is None and m.inv_owner[name] is not None:
            m.warnings.append(f"invention {name}: owner has no page")
    m.stats["inventions"] = len(wiki.inventions)
    return m


def check_tech_graph(wiki, mapping, graph):
    """Tech-graph node ids vs the model's inventions; owners that disagree. Returns problems."""
    bad = []
    ids, mine = set(graph["nodes"]) | set(graph.get("dropped", {})), set(wiki.inventions)
    for x in sorted(mine - ids)[:20]:
        bad.append(f"invention {x} missing from the tech graph")
    for x in sorted(ids - mine)[:20]:
        bad.append(f"tech graph node {x} unknown to the model")
    for i, n in graph["nodes"].items():
        own = mapping.inv_owner.get(i)
        if own is None and n["owner"]:
            bad.append(f"node {i}: graph owner {n['owner']} but the model has none")
        elif own is not None and n["owner"]:
            e = wiki.get(*own)
            if e.cfg_name != n["owner"]:
                mapping.warnings.append(
                    f"node {i}: graph owner {n['owner']} != model owner {e.cfg_name}"
                )
    return bad


# ------------------------------------------------------------------------------ page context


def _num(v):
    """Compact number text: ints plain, floats to 6 significant digits."""
    if isinstance(v, bool):
        return str(v)
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        if abs(v - round(v)) < 1e-9:
            return str(round(v))
        return f"{v:.6g}"
    return str(v)


class Ctx:
    """Everything a template needs for one page (language, relative prefixes, helpers)."""

    def __init__(self, site, lang, page_path):
        self.site = site
        self.wiki = site.wiki
        self.lang = lang
        self.S = site.S[lang]
        self.page_path = page_path
        depth = page_path.count("/")
        self.L = "../" * depth  # path to this language's root
        self.R = self.L + "../"  # path to the site root
        self.A = self.R + "assets/"

    # -- text
    def label(self, key):
        return self.wiki.label(self.lang, key) if key else ""

    def name(self, x):
        e = self.ent(x)
        if e is None:
            return getattr(x, "slug", "")
        return e.names.get(self.lang) or e.names.get("en") or e.cfg_name

    def ent(self, x):
        if hasattr(x, "cfg_name"):
            return x
        try:
            return self.wiki.get(x.kind, x.slug)
        except KeyError:
            return None

    def fmt(self, v):
        if v is None:
            return ""
        if isinstance(v, (list, tuple)):
            return ", ".join(self.fmt(x) for x in v)
        if isinstance(v, dict):
            return ", ".join(f"{k}: {self.fmt(x)}" for k, x in v.items())
        return _num(v)

    def rowlabel(self, r):
        if not r.label_key:
            return r.cfg_key
        t = self.label(r.label_key)
        if r.label_key.removesuffix("_a") == "K_MENU_Energy":
            t = t.rstrip(": ") + f" ({self.S['hp']}):"
        return t

    def val(self, r):
        if r.value is None:
            return ""
        s = r.marker + self.fmt(r.value)
        if r.unit_key:
            s += " " + self.label(r.unit_key)
        return s

    def unv_title(self, r):
        note = re.sub(r"^UNVERIFIED:\s*", "", r.note)
        return f"{self.S['unverified']}: {note}" if note else self.S["unverified"]

    def letters(self, mask):
        return self.wiki.target_letters(self.lang, mask)

    def target_title(self, mask):
        parts = [self.S["ground"]] if mask & 1 else []
        if mask & 2:
            parts.append(self.S["air"])
        return ", ".join(parts)

    def role_title(self, e):
        if e.kind == "building":
            return self.S["building_role"].get(e.role_name, "")
        if e.kind == "unit":
            return self.S["unit_class"].get(e.unit_class, "")
        return ""

    def description(self, e):
        t = e.description.get(self.lang) or e.description.get("en") or ""
        return [p.strip() for p in t.splitlines() if p.strip()]

    def desc_fallback(self, e):
        return self.lang in e.description_fallback and bool(e.description.get(self.lang))

    # -- links
    def href(self, link):
        e = self.ent(link)
        if e is None:
            return ""
        if e.kind == "upgrade":
            pj = e.links.get("project") or []
            if not pj:
                return ""
            e = self.wiki.get(pj[0].kind, pj[0].slug)
        if e.kind not in KIND_DIR:
            return ""
        if e.kind == "building" and e.level > 0:  # upgrade levels live on the base building's page
            return f"{self.L}buildings/{e.chain_base.slug}.html#{self.level_id(e)}"
        return f"{self.L}{KIND_DIR[e.kind]}/{e.slug}.html"

    @staticmethod
    def level_id(b):
        return "base" if b.level == 0 else f"upgrade-{b.level}"

    def level_title(self, b):
        return self.S["level_base"] if b.level == 0 else self.S["level_up"].format(b.level)

    def levels(self, e):
        """Chain members shown on ``e``'s page (only a base building with upgrades has several)."""
        if e.kind == "building" and e.level == 0 and len(e.chain) > 1:
            return self.wiki.chain_members(e)
        return []

    def host_key(self, x):
        """(kind, slug) of the page that shows ``x`` (an upgrade level -> its base building)."""
        e = self.ent(x)
        if e is None:
            return None
        if e.kind == "upgrade":
            pj = e.links.get("project") or []
            return (pj[0].kind, pj[0].slug) if pj else None
        if e.kind == "building":
            return ("building", e.chain_base.slug)
        return (e.kind, e.slug)

    def starmap(self, e):
        """Star-system page data: background + planets at their in-game positions, or None."""
        r = self.site.mapping.ref(e)
        if e.kind != "system" or not r.starmap:
            return None
        from .starmap import CROP_H

        w = self.wiki
        planets = []
        for pos in w.starmap_layout(e):
            pe = w.get("planet", pos["slug"])
            planets.append(
                {
                    "href": f"{self.L}planets/{pe.slug}.html",
                    "name": self.name(pe),
                    "img": self.asset(self.site.mapping.ref(pe).planet),
                    "left": round(pos["x"] / STARMAP_W * 100, 3),
                    "top": round(pos["y"] / CROP_H * 100, 3),
                    "size": round(STARMAP_ANIM / STARMAP_W * 100, 3),
                }
            )
        return {"bg": self.asset(r.starmap), "ratio": f"{STARMAP_W} / {CROP_H}", "planets": planets}

    def ambiguous(self, links):
        """{(kind, slug)} of the links in ONE list whose display name is shared with a link of
        another faction (alien Heavy Factory vs human Heavy Factory): only those get a side marker."""
        by_name = {}
        for lk in links:
            e = self.ent(lk)
            if e is None or not getattr(e, "faction", None):
                continue
            by_name.setdefault(self.name(e).casefold(), {}).setdefault(e.faction, []).append(
                (e.kind, e.slug)
            )
        out = set()
        for facs in by_name.values():
            if len(facs) > 1:
                for keys in facs.values():
                    out.update(keys)
        return out

    def side(self, link, amb):
        """' (Alien)' for a link in ``amb`` (see :meth:`ambiguous`), else ''."""
        e = self.ent(link)
        if e is None or not amb or (e.kind, e.slug) not in amb:
            return ""
        return f"({self.S[e.faction]})"

    def planet_map(self, e):
        """Planet page data: map preview + landing / ore markers (percent of the image), or None."""
        r = self.site.mapping.ref(e)
        i = r.mapinfo
        if e.kind != "planet" or not r.mapimg or not i:
            return None
        w, h = i["w"], i["h"]

        def pts(lst):
            return [
                {"left": round((x + 0.5) / w * 100, 3), "top": round((y + 0.5) / h * 100, 3)}
                for x, y in lst
            ]

        return {
            "img": self.asset(r.mapimg),
            "w": w,
            "h": h,
            "land": pts(i["land"]),
            "ore": pts(i["ore"]),
        }

    def nav(self):
        out = [(self.S["kinds_pl"][k], f"{self.L}{KIND_DIR[k]}/index.html") for k in PAGE_KINDS]
        out.append((self.S["techtree"], f"{self.L}techtree/index.html"))
        return out

    def lang_links(self):
        out = []
        for code in self.site.langs:
            name = LANG_NAME.get(code, code)
            out.append(
                (code, name, "" if code == self.lang else f"{self.R}{code}/{self.page_path}")
            )
        return out

    def link_groups(self, e):
        groups = []
        members = self.wiki.chain_members(e) if e.kind == "building" else [e]
        mine = self.host_key(e)
        for role in ROLE_ORDER:
            links, seen = [], set()
            for m in members:
                for lk in m.links.get(role, []):
                    key = self.host_key(lk)  # an upgrade and its base building share one page
                    if key in seen or not self.href(lk) or self.host_key(lk) == mine:
                        continue
                    seen.add(key)
                    links.append(lk)
            if links:
                groups.append((self.S["roles"][role], links))
        return groups

    # -- art
    def asset(self, rel):
        return self.A + rel if rel else ""

    def thumb_src(self, e):
        r = self.site.mapping.ref(e)
        return self.asset(r.icon or r.planet)

    def media(self, e):
        r = self.site.mapping.ref(e)
        S = self.S
        out = []
        if r.icon:
            out.append((self.asset(r.icon), S["cap_icon"], "icon"))
        if r.planet:
            out.append((self.asset(r.planet), S["cap_planet"], "planet"))
        if r.sprite:
            cap = S["cap_sprite"] if e.kind == "building" else S["cap_unit"]
            w, h = self.site.sizes.get(r.sprite, (0, 0))
            big = e.kind == "unit" or max(w, h) < 100  # tiny in-game sprites are shown 2x
            out.append((self.asset(r.sprite), cap, "sprite x2" if big else "sprite"))
        if r.still:
            out.append((self.asset(r.still), S["cap_still"], "still"))
        return out

    # -- tech tree hover cards
    def card(self, e):
        rows = [(self.rowlabel(r).rstrip(": "), self.val(r)) for r in e.panel.left if r.value]
        for r in e.panel.right:
            if r.value is not None and not r.items:
                rows.append((self.rowlabel(r).rstrip(": "), self.val(r)))
        p = e.panel
        if p.costs:
            rows.append(
                (
                    self.label(p.costs_label_key).rstrip(": "),
                    " / ".join(self.fmt(c.value) for c in p.costs),
                )
            )
        if p.research:
            rows.append((self.rowlabel(p.research).rstrip(": "), self.val(p.research)))
        return [[a, b] for a, b in rows]


# ------------------------------------------------------------------------------ the site


class Site:
    def __init__(self, wiki, art_maps, langs=("en", "ru")):
        self.wiki = wiki
        self.langs = [la for la in langs]
        self.S = {la: chrome(la) for la in self.langs}
        self.art_maps = art_maps
        self.mapping = build_mapping(wiki, art_maps)
        self.env = jinja2.Environment(
            loader=jinja2.FileSystemLoader(str(HERE / "templates")),
            autoescape=jinja2.select_autoescape(["html"]),
            keep_trailing_newline=True,
        )
        self.written = {la: [] for la in self.langs}
        self.sizes = {}  # sprite rel path -> (w, h)

    def read_sizes(self, out):
        from PIL import Image

        for r in self.mapping.refs.values():
            if r.sprite and r.sprite not in self.sizes:
                try:
                    with Image.open(Path(out) / "assets" / r.sprite) as im:
                        self.sizes[r.sprite] = im.size
                except OSError:
                    pass

    # -- output helpers
    def write(self, out, lang, page_path, html):
        dest = Path(out) / lang / page_path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(html, encoding="utf-8", newline="\n")
        self.written[lang].append(page_path)

    def render(self, out, lang, page_path, template, title, **kw):
        c = Ctx(self, lang, page_path)
        html = self.env.get_template(template).render(
            c=c, S=c.S, lang=lang, L=c.L, R=c.R, title=title, page_path=page_path, **kw
        )
        self.write(out, lang, page_path, html)

    # -- pages
    def entity_pages(self, out, lang):
        for kind in PAGE_KINDS:
            for e in self.wiki.by_kind[kind]:
                c = Ctx(self, lang, "x/x")
                if kind == "building" and e.level > 0:
                    # old slug of an upgrade level: a short page pointing at the combined one
                    base = self.wiki.get("building", e.chain_base.slug)
                    self.render(
                        out,
                        lang,
                        f"buildings/{e.slug}.html",
                        "upgrade_stub.html",
                        c.name(e),
                        e=e,
                        base=base,
                        target=f"{base.slug}.html#{Ctx.level_id(e)}",
                    )
                    continue
                self.render(
                    out,
                    lang,
                    f"{KIND_DIR[kind]}/{e.slug}.html",
                    "entity.html",
                    c.name(e),
                    e=e,
                )

    def _find(self, e, keys):
        for r in e.panel.left + e.panel.right:
            if r.label_key and r.label_key.removesuffix("_a") in keys:
                return r
        return None

    def _cell(self, c, e, keys):
        r = self._find(e, keys)
        if r is None or r.value is None:
            return ("", "")
        return (c.fmt(r.value), r.value)

    def _cost_cells(self, e):
        if not e.panel.costs:
            return [("", "")] * 4
        return [(_num(k.value), k.value) for k in e.panel.costs[:4]]

    def _columns(self, c, kind):
        """[(header, getter(entity) -> (text, sort value))] per class."""
        S = c.S
        ore = [
            c.label(k).rstrip(": ")
            for k in ("K_CONS_RESOURCE", "K_CONS_RESOURCE1", "K_CONS_RESOURCE2", "K_CONS_RESOURCE3")
        ]

        def cost(i):
            return lambda e: self._cost_cells(e)[i]

        def lab(key, hp=False):
            t = c.label(key).rstrip(": ")
            return f"{t} ({S['hp']})" if hp else t

        def roww(*keys):
            return lambda e: self._cell(c, e, keys)

        if kind == "unit":
            cols = [
                (S["col_type"], lambda e: (c.role_title(e), c.role_title(e).lower())),
                (lab("K_MENU_Speed"), roww("K_MENU_Speed")),
                (lab("K_MENU_Energy", True), roww("K_MENU_Energy")),
                (lab("K_MENU_Sight"), roww("K_MENU_Sight")),
                (lab("K_MENU_ProdTime"), roww("K_MENU_ProdTime")),
            ]
            cols += [(ore[i], cost(i)) for i in range(4)]
            cols.append(
                (S["col_weapons"], lambda e: (str(len(e.panel.weapons)), len(e.panel.weapons)))
            )
            return cols
        if kind == "building":
            cols = [
                (S["col_type"], lambda e: (c.role_title(e), c.role_title(e).lower())),
                (S["levels"], lambda e: (str(len(e.chain)), len(e.chain))),
                (S["col_power"], roww("K_MENU_EnergyProd", "K_MENU_EnergyReq")),
                (lab("K_MENU_Energy", True), roww("K_MENU_Energy")),
                (lab("K_MENU_BuildTime"), roww("K_MENU_BuildTime")),
            ]
            cols += [(ore[i], cost(i)) for i in range(4)]
            return cols
        if kind == "project":
            cols = [
                (
                    S["research_time"].rstrip(": "),
                    lambda e: (
                        (c.val(e.panel.research), e.panel.research.value)
                        if e.panel.research
                        else ("", "")
                    ),
                )
            ]
            cols += [(ore[i], cost(i)) for i in range(4)]
            cols.append(
                (S["col_unlocks"], lambda e: (str(n := len(e.links.get("unlocks", []))), n))
            )
            return cols
        if kind == "weapon":
            return [
                (lab("K_MENU_Power"), roww("K_MENU_Power")),
                (lab("K_MENU_RangeMin"), roww("K_MENU_RangeMin")),
                (lab("K_MENU_RangeMax"), roww("K_MENU_RangeMax")),
                (
                    S["col_users"],
                    lambda e: (
                        str(
                            n := len(e.links.get("used_by_units", []))
                            + len(e.links.get("used_by_buildings", []))
                        ),
                        n,
                    ),
                ),
            ]
        if kind == "planet":

            def sysname(e):
                s = e.links.get("system") or []
                t = c.name(s[0]) if s else ""
                return (t, t.lower())

            return [
                (S["col_system"], sysname),
                (S["col_unlocks"], lambda e: (str(n := len(e.links.get("unlocks", []))), n)),
            ]
        return [
            (S["col_planets"], lambda e: (str(n := len(e.links.get("planets", []))), n)),
            (S["col_unlocks"], lambda e: (str(n := len(e.links.get("unlocks", []))), n)),
        ]

    def class_index(self, out, lang, kind):
        c = Ctx(self, lang, f"{KIND_DIR[kind]}/index.html")
        cols = self._columns(c, kind)
        ents = self.wiki.listed(kind)
        facs = [f for f in ("alien", "human") if any(e.faction == f for e in ents)]
        groups = [(f, c.S[f], [e for e in ents if e.faction == f]) for f in facs]
        rest = [e for e in ents if e.faction not in facs]
        if rest:
            groups.append((None, c.S["shared"] if facs else c.S["all"], rest))
        sections = []
        for fac, title, items in groups:
            rows = [
                {
                    "slug": e.slug,
                    "name": c.name(e),
                    "thumb": c.asset(self.mapping.ref(e).mapthumb),
                    "cells": [fn(e) for _, fn in cols],
                }
                for e in items
            ]
            sections.append({"faction": fac, "title": title, "ents": items, "rows": rows})
        self.render(
            out,
            lang,
            f"{KIND_DIR[kind]}/index.html",
            "class_index.html",
            c.S["kinds_pl"][kind],
            kind=kind,
            sections=sections,
            headers=[h for h, _ in cols],
            total=len(ents),
            map_thumbs=kind == "planet" and any(r["thumb"] for s in sections for r in s["rows"]),
        )

    def home(self, out, lang):
        c = Ctx(self, lang, "index.html")
        cards = [
            (c.S["kinds_pl"][k], f"{KIND_DIR[k]}/index.html", len(self.wiki.listed(k)))
            for k in PAGE_KINDS
        ]
        trees = [
            (c.S["tt_faction"].format(c.S[f]), f"techtree/{f}.html") for f in ("alien", "human")
        ]
        self.render(
            out,
            lang,
            "index.html",
            "home.html",
            c.S["home"],
            cards=cards,
            trees=trees,
            missing=self.wiki.missing.get(lang, []) if lang != "en" else [],
        )

    # -- tech tree
    def tech_trees(self, out, langs):
        w = self.wiki
        graph = techtree.build_graph(w.cfg)
        self.tt_problems = check_tech_graph(w, self.mapping, graph)
        per = {}
        for f in ("alien", "human"):
            sub = techtree.faction_graph(graph, f)
            per[f] = (sub, techtree.layout(sub))
        self.tt_stats = {f: (len(per[f][0]["nodes"]), len(per[f][0]["edges"])) for f in per}
        for lang in langs:
            c0 = Ctx(self, lang, "techtree/x.html")
            trees = []
            for f, (sub, lay) in per.items():
                hrefs, icons, labels, cards = {}, {}, {}, {}
                for i in sub["nodes"]:
                    pg = self.mapping.inv_page.get(i)
                    ow = self.mapping.inv_owner.get(i)
                    pe = w.get(*pg) if pg else None
                    oe = w.get(*ow) if ow else None
                    if pe is not None:
                        hrefs[i] = c0.href(pe)
                    show = oe if oe is not None else pe
                    if show is not None:
                        labels[i] = c0.name(show)
                        ic = self.mapping.ref(show).icon or (
                            self.mapping.ref(pe).icon if pe is not None else ""
                        )
                        if ic:
                            icons[i] = c0.asset(ic)
                        card_src = show if (show.panel.left or show.panel.costs) else (pe or show)
                        cards[i] = c0.card(card_src)
                title = c0.S["tt_faction"].format(c0.S[f])
                frag = techtree.render_page(
                    sub,
                    lay,
                    f,
                    icon_map=icons,
                    lang_labels=labels,
                    hrefs=hrefs,
                    cards=cards,
                    title=title,
                    ui=c0.S.get("tt_ui") or None,
                    standalone=False,
                    asset_prefix=c0.A,
                )
                self.render(
                    out,
                    lang,
                    f"techtree/{f}.html",
                    "techtree_page.html",
                    title,
                    fragment=Markup(frag),
                    note=c0.S["tt_squads"],
                )
                trees.append((title, f"{f}.html", f"{len(sub['nodes'])}"))
            self.render(
                out,
                lang,
                "techtree/index.html",
                "techtree_index.html",
                c0.S["techtree"],
                trees=trees,
                note=c0.S["tt_squads"],
            )

    # -- search
    def search_index(self, lang):
        c = Ctx(self, lang, "x/x")
        other = [la for la in self.langs if la != lang]
        out = []
        for kind in PAGE_KINDS:
            for e in self.wiki.listed(kind):
                names = [c.name(e)] + [e.names.get(la, "") for la in other]
                names += [e.slug, e.cfg_name.removeprefix("N_")]
                for m in c.levels(e)[1:]:  # upgrade levels are found through the base page
                    names += [m.slug, m.cfg_name.removeprefix("N_")]
                words = []
                for n in names:
                    n = n.lower()
                    words += [n] + n.replace("_", " ").split()
                r = self.mapping.ref(e)
                out.append(
                    {
                        "n": c.name(e),
                        "k": c.S["kinds"][kind],
                        "f": e.faction or "",
                        "u": f"{KIND_DIR[kind]}/{e.slug}.html",
                        "i": r.icon or r.planet,
                        "s": "|".join(dict.fromkeys(w for w in words if w)),
                    }
                )
        return out

    def write_search(self, out, lang):
        data = self.search_index(lang)
        blob = json.dumps(data, ensure_ascii=False, separators=(",", ":"))
        d = Path(out) / lang
        (d / "search.json").write_text(blob, encoding="utf-8", newline="\n")
        (d / "search.js").write_text(f"window.MH_INDEX={blob};\n", encoding="utf-8", newline="\n")
        return len(data)


@dataclass
class SiteReport:
    out_dir: str = ""
    langs: tuple = ()
    pages: dict = field(default_factory=dict)
    search_entries: int = 0
    errors: list = field(default_factory=list)
    warnings: list = field(default_factory=list)
    seconds: float = 0.0
    mapping: Mapping | None = None
    tech: dict = field(default_factory=dict)


def load_art_maps(art_dir):
    """``art_dir/assets.json`` (written by ``python -m src.formats.wiki.art``) -> art_maps."""
    return json.loads((Path(art_dir) / "assets.json").read_text(encoding="utf-8"))


def _copy_assets(out, art_dir, art_maps):
    dest = Path(out) / "assets"
    dest.mkdir(parents=True, exist_ok=True)
    for f in sorted((HERE / "static").iterdir()):
        if f.is_file():
            shutil.copyfile(f, dest / f.name)
    if art_dir is None:
        return
    rels = {rel for mp in art_maps.values() for rel in mp.values() if isinstance(rel, str)}
    for rel in sorted(rels):
        src = Path(art_dir) / rel
        if src.is_file():
            (dest / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dest / rel)


def build_site(wiki, art_maps, out_dir, langs=("en", "ru"), art_dir=None, strict=True, clean=True):
    """Write the whole site under ``out_dir``; returns a :class:`SiteReport`.

    ``art_dir``: where the art maps' relative paths live (copied to ``out_dir/assets``); None when
    ``out_dir/assets`` already holds them.  ``strict``: raise :class:`SiteBuildError` when the key
    spaces do not reconcile (the site is still written first so it can be inspected).
    """
    t0 = time.time()
    langs = tuple(langs)
    for la in langs:
        if la not in wiki.langs:
            raise SiteBuildError([f"language {la!r} not loaded in the model ({wiki.langs})"])
    out = Path(out_dir)
    if clean and out.exists():
        for child in out.iterdir():
            if child.name == "assets" and art_dir is None:
                continue  # assets were prepared in place
            shutil.rmtree(child) if child.is_dir() else child.unlink()
    out.mkdir(parents=True, exist_ok=True)
    site = Site(wiki, art_maps, langs)
    _copy_assets(out, art_dir, art_maps)
    site.read_sizes(out)
    for lang in langs:
        site.home(out, lang)
        for kind in PAGE_KINDS:
            site.class_index(out, lang, kind)
        site.entity_pages(out, lang)
    site.tech_trees(out, langs)
    n_search = 0
    for lang in langs:
        n_search = site.write_search(out, lang)
    root = site.env.get_template("root.html").render(
        title=chrome("en")["site"], langs=[(la, LANG_NAME.get(la, la)) for la in langs]
    )
    (out / "index.html").write_text(root, encoding="utf-8", newline="\n")

    rep = SiteReport(out_dir=str(out), langs=langs, mapping=site.mapping)
    rep.pages = {la: len(site.written[la]) for la in langs}
    rep.search_entries = n_search
    rep.errors = list(site.mapping.errors) + list(site.tt_problems)
    rep.warnings = list(site.mapping.warnings)
    rep.tech = dict(site.tt_stats)
    rep.seconds = time.time() - t0
    if strict and rep.errors:
        raise SiteBuildError(rep.errors)
    return rep


# ------------------------------------------------------------------------------ checks


class _Refs(HTMLParser):
    def __init__(self):
        super().__init__()
        self.refs = []

    def handle_starttag(self, tag, attrs):
        for k, v in attrs:
            if v and (k in ("href", "src", "xlink:href") or (k == "data" and tag == "object")):
                self.refs.append(v)


_EXTERNAL = re.compile(r"^(?:[a-z][a-z0-9+.-]*:|//|#)", re.IGNORECASE)
_TT_DATA = re.compile(r'<script type="application/json" class="tt-data">(.*?)</script>', re.DOTALL)


def _local_ref(ref):
    ref = ref.split("#", 1)[0].split("?", 1)[0]
    return None if not ref or _EXTERNAL.match(ref) else ref


def check_links(out_dir):
    """Every relative href/src/xlink:href in every page (and the tech-tree node hrefs) resolves."""
    out = Path(out_dir)
    bad, n = [], 0
    for p in sorted(out.rglob("*.html")):
        text = p.read_text(encoding="utf-8")
        parser = _Refs()
        parser.feed(text)
        refs = list(parser.refs)
        m = _TT_DATA.search(text)
        if m:
            refs += [d["h"] for d in json.loads(m.group(1)).values() if d.get("h")]
        for r in refs:
            lr = _local_ref(r)
            if lr is None:
                continue
            n += 1
            if not (p.parent / lr).resolve().exists():
                bad.append(f"{p.relative_to(out).as_posix()}: dangling {r}")
    if n == 0:
        bad.append("no relative links found (empty site?)")
    return bad


def check_search(wiki, out_dir, langs):
    """search.json parses, covers every page entity once, and every entry's url resolves."""
    bad = []
    for lang in langs:
        d = Path(out_dir) / lang
        try:
            data = json.loads((d / "search.json").read_text(encoding="utf-8"))
        except (OSError, ValueError) as ex:
            bad.append(f"{lang}/search.json: {ex}")
            continue
        urls = [e.get("u") for e in data]
        want = {f"{KIND_DIR[k]}/{e.slug}.html" for k in PAGE_KINDS for e in wiki.listed(k)}
        if len(urls) != len(set(urls)):
            bad.append(f"{lang}/search.json: duplicate urls")
        for miss in sorted(want - set(urls))[:10]:
            bad.append(f"{lang}/search.json: missing {miss}")
        for u in sorted(set(urls) - want)[:10]:
            bad.append(f"{lang}/search.json: unknown {u}")
        for e in data:
            if not e.get("n") or not e.get("s"):
                bad.append(f"{lang}/search.json: empty entry {e.get('u')}")
            if not (d / e["u"]).is_file():
                bad.append(f"{lang}/search.json: {e['u']} has no page")
            if e.get("i") and not (Path(out_dir) / "assets" / e["i"]).is_file():
                bad.append(f"{lang}/search.json: icon {e['i']} missing")
        if not (d / "search.js").is_file():
            bad.append(f"{lang}/search.js missing")
    return bad


def check_languages(out_dir, langs):
    """Every language tree holds the identical set of pages."""
    out = Path(out_dir)
    sets = {
        la: {p.relative_to(out / la).as_posix() for p in (out / la).rglob("*") if p.is_file()}
        for la in langs
    }
    bad = []
    first = langs[0]
    for la in langs[1:]:
        for x in sorted(sets[first] - sets[la])[:10]:
            bad.append(f"{la}: missing {x} (present in {first})")
        for x in sorted(sets[la] - sets[first])[:10]:
            bad.append(f"{first}: missing {x} (present in {la})")
    for la in langs:
        if not sets[la]:
            bad.append(f"{la}: no pages")
    return bad


def check_art_shown(wiki, mapping, out_dir, langs):
    """Every entity with mapped art shows each asset (icon, sprite, still, planet, star map).

    Upgrade levels of a building are checked against their base building's page (they have none
    of their own).  A still is mapped only where it is shown (``build_mapping`` drops stills that
    are shared by many entities, and project stills), so "mapped" and "shown" are the same set.
    """
    bad = []
    for lang in langs:
        for kind in PAGE_KINDS:
            for e in wiki.by_kind[kind]:
                r = mapping.ref(e)
                want = [x for x in (r.icon, r.sprite, r.still, r.planet, r.starmap, r.mapimg) if x]
                if not want:
                    continue
                slug = e.chain_base.slug if kind == "building" else e.slug
                p = Path(out_dir) / lang / KIND_DIR[kind] / f"{slug}.html"
                text = p.read_text(encoding="utf-8") if p.is_file() else ""
                for x in want:
                    if f'src="../../assets/{x}"' not in text:
                        bad.append(f"{lang}/{kind}/{e.slug}: art {x} not shown")
    return bad


def check_pages_exist(wiki, out_dir, langs):
    bad = []
    for lang in langs:
        for kind in PAGE_KINDS:
            for e in wiki.by_kind[kind]:
                if not (Path(out_dir) / lang / KIND_DIR[kind] / f"{e.slug}.html").is_file():
                    bad.append(f"{lang}/{kind}/{e.slug}: page missing")
    return bad


def run_checks(wiki, mapping, out_dir, langs=("en", "ru")):
    """All site checks -> ``{check name: [problems]}`` (all lists empty = pass)."""
    langs = tuple(langs)
    return {
        "mapping": list(mapping.errors),
        "pages": check_pages_exist(wiki, out_dir, langs),
        "links": check_links(out_dir),
        "search": check_search(wiki, out_dir, langs),
        "languages": check_languages(out_dir, langs),
        "art_shown": check_art_shown(wiki, mapping, out_dir, langs),
    }


# ------------------------------------------------------------------------------ test runner


def _main(argv=None):  # pragma: no cover -- retail runner
    import argparse

    from . import source
    from .art import (
        ArtSource,
        SpriteStore,
        extract_building_sprites,
        extract_icons,
        extract_planet_maps,
        extract_planets,
        extract_stills,
        extract_unit_sprites,
    )
    from .model import build

    ap = argparse.ArgumentParser(description="Build the wiki site from retail data (test runner).")
    ap.add_argument("--out", default=str(HERE.parents[2] / "tmp" / "wiki_site"))
    ap.add_argument("--game")
    ap.add_argument("--ru")
    ap.add_argument("--no-stills", action="store_true")
    ap.add_argument("--langs", default="en,ru")
    a = ap.parse_args(argv)
    t0 = time.time()
    out = Path(a.out)
    langs = tuple(a.langs.split(","))
    game = a.game or source.default_game()
    ru = a.ru or source.default_ru()
    wiki = build(source.load(game, ru=ru))
    t1 = time.time()
    assets = out.parent / (out.name + "_art")
    src = ArtSource(game)
    store = SpriteStore(src)
    pm, pt, pi = extract_planet_maps(src, assets)
    art = {
        "planetmaps": pm,
        "planetthumbs": pt,
        "planetmapinfo": pi,
        "icons": extract_icons(src, assets),
        "buildings": extract_building_sprites(src, assets, store),
        "units": extract_unit_sprites(src, assets, store),
        "planets": extract_planets(src, assets),
        "stills": {} if a.no_stills else extract_stills(src, assets),
    }
    t2 = time.time()
    rep = build_site(wiki, art, out, langs=langs, art_dir=assets, strict=False)
    shutil.rmtree(assets, ignore_errors=True)
    t3 = time.time()
    problems = run_checks(wiki, rep.mapping, out, langs)
    t4 = time.time()
    print(f"model {t1 - t0:.1f}s  art {t2 - t1:.1f}s  site {t3 - t2:.1f}s  checks {t4 - t3:.1f}s")
    print("pages:", rep.pages, "search entries:", rep.search_entries, "tech:", rep.tech)
    print("art notes:", src.notes[:6])
    print("mapping warnings:", *rep.warnings, sep="\n  ")
    for k, v in rep.mapping.stats.items():
        if isinstance(v, list):
            print(f"  {k}: {v[:6]}")
    for k, v in problems.items():
        print(f"check {k}: {'OK' if not v else str(len(v)) + ' PROBLEMS'}")
        for x in v[:8]:
            print("   ", x)
    print("open:", (out / "index.html").resolve())
    return 1 if any(problems.values()) else 0


if __name__ == "__main__":
    raise SystemExit(_main())
