"""Keyword grammar: per-keyword value types and staging targets.

Single source of truth, transcribed from the cfg grammar notes §5–6
(RE'd from ``cfg::HandleConfigEntry`` / ``cfg::ObjectInit``). Every keyword the
retail file uses in a section body is here with (a) how its argument(s) parse
and (b) which record field they land in; :func:`new_record` seeds a staging
record with the ObjectInit defaults so the model is complete (the decompiler
will omit fields equal to these; the compiler re-applies them).

Argument kinds
--------------
================  =========================================  ============================
kind              wire syntax                                storage
================  =========================================  ============================
``int``           bare int32                                 scalar
``f32``           bare float32-widened-to-double             scalar (f32-quantized)
``str``           ``"quoted"``                               scalar
``flag``          (no arg — bare keyword)                    scalar := 1
``slot_str``      ``<int slot> "name"``                      ``field[slot] = name``
``slot_int``      ``<int slot> <int>``                       ``field[slot] = v``
``slot_f32``      ``<int slot> <f32>``                       ``field[slot] = v``
``cap``           ``<int slot> "name" <f32>``                ``field[slot] = (name, v)``
``res``           ``<int v> "name"``                         ``field.append((name, v))``
``depend``        ``"name"``                                 ``field.append(name)``
``area``          ``<int col> <int>…``                       ``area[row][col] = v_row``
``bank_range``    ``<int lo> <int hi>``                      ``field |= {lo..hi}``  (inclusive)
``source4``       ``<int> <int> <int> <int>``                ``field = [a, b, c, d]``
``bank_sprite``   ``"name" <int frames>``                    BANK sprite list (special)
================  =========================================  ============================

Shared arrays (one field written by two keywords — reproduce faithfully):
``PRODUCTION``/``TRANSPORT`` → ``unit_name``; ``PRODUCTION_TIME``/``TRANSPORT_QUANT``
→ ``unit_quant``; ``EXTRACT``/``FUEL`` → ``extract_fuel``.
"""
from __future__ import annotations

import struct

# argument-kind constants
INT, F32, STR, FLAG = "int", "f32", "str", "flag"
SLOT_STR, SLOT_INT, SLOT_F32, CAP = "slot_str", "slot_int", "slot_f32", "cap"
RES, DEPEND, AREA, BANK_RANGE, SOURCE4 = "res", "depend", "area", "bank_range", "source4"
BANK_SPRITE = "bank_sprite"


def f32(x: float) -> float:
    """Quantize a Python float through IEEE-754 binary32, as the game does."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


SECTION_OPENERS = {
    "BANK", "ANIM", "WEAPON", "UNIT", "BUILDING", "PROGRESS", "PLANET",
    "SYSTEM", "UPGRADE", "PROJECT", "STONE", "TREE",
}

# Top-level global modifiers: all _MUL are f32, all _ADD are int EXCEPT
# UNIT_SPEED_ADD which is f32.
_MOD_STEMS = [
    "WEAPON_POWER", "UNIT_SPEED", "UNIT_ENERGY", "UNIT_SIGHT", "UNIT_BUILD_TIME",
    "UNIT_RESOURCE", "BUILDING_ENERGY", "BUILDING_POWER", "BUILDING_SIGHT",
    "BUILDING_BUILD_TIME", "BUILDING_HUMAN", "BUILDING_BUILDER",
]
MODIFIERS = {}
for _s in _MOD_STEMS:
    MODIFIERS[_s + "_MUL"] = F32
    MODIFIERS[_s + "_ADD"] = F32 if _s == "UNIT_SPEED" else INT
MODIFIER_DEFAULTS = {key: (1.0 if key.endswith("_MUL") else 0.0) for key in MODIFIERS}

# Per-class body grammar: keyword -> (kind, field).
CLASS_GRAMMAR = {
    "ANIM": {
        "SPRITE": (STR, "sprite"), "LENGTH": (INT, "length"),
        "TIME": (F32, "time"), "STEP": (INT, "step"),
        "REPEAT": (INT, "repeat"), "CYCLIC": (FLAG, "cyclic"),
    },
    "WEAPON": {
        "TYPE": (STR, "type"), "FIRE_EXPLO": (STR, "fire_explo"),
        "TARGET_EXPLO": (STR, "target_explo"), "SMOKE_SPRITE": (STR, "smoke_sprite"),
        "HOMING": (STR, "homing"), "TARGET": (STR, "target"),
        "SOUND_FIRE": (STR, "sound_fire"), "SOUND_TARGET": (STR, "sound_target"),
        "AMMO": (INT, "ammo"), "POCKET": (INT, "pocket"),
        "RANGE_MIN": (INT, "range_min"), "RANGE_MAX": (INT, "range_max"),
        "POWER": (INT, "power"), "LENGTH": (INT, "length"),
        "MISSING": (INT, "missing"), "FIRE_RANGE": (INT, "fire_range"),
        "SHORT_TIME": (F32, "short_time"), "LONG_TIME": (F32, "long_time"),
        "SPEED": (F32, "speed"), "EXPLO_TIME": (F32, "explo_time"),
        "SMOKE_TIME": (F32, "smoke_time"),
        "BULLET_ANIM": (SLOT_STR, "bullet_anims"),
        "PROBABILYTY": (SLOT_INT, "probability"),  # sic — misspelled in binary
    },
    "UNIT": {
        "INVENTION": (STR, "invention"), "SPRITE": (STR, "sprite"),
        "SPRITE_SHADOW": (STR, "sprite_shadow"), "SPRITE_TYPE": (STR, "sprite_type"),
        "FRAME": (STR, "frame"), "FRAME_2": (STR, "frame_2"),
        "EQUIVALENT": (STR, "equivalent"), "ANIM_EXPLO": (STR, "anim_explo"),
        "INFO_TXT": (STR, "info_txt"), "INFO_FLC": (STR, "info_flc"),
        "INDEPENDENT": (STR, "independent"), "TYPE": (STR, "type"),
        "ICON": (STR, "icon"), "SOUND_EXPLO": (STR, "sound_explo"),
        "SOUND_MOVE": (STR, "sound_move"), "AIUNIT": (STR, "aiunit"),
        "SOLDIER_TYPE": (STR, "soldier_type"),
        "ARMOR": (INT, "armor"), "ENERGY": (INT, "energy"),
        "P_1": (INT, "p_1"), "P_2": (INT, "p_2"), "P_3": (INT, "p_3"),
        "P_4": (INT, "p_4"), "P_5": (INT, "p_5"),
        "SIGHT": (INT, "sight"), "HUMAN": (INT, "human"),
        "TRACE": (INT, "trace"), "AILEVEL": (INT, "ai_level"),
        "STEP_SPEED": (F32, "step_speed"), "TURN_SPEED": (F32, "turn_speed"),
        "BUILD_TIME": (F32, "build_time"),
        "RESOURCE": (RES, "resources"),
        "WEAPON": (SLOT_STR, "weapons"),
    },
    "BUILDING": {
        "INVENTION": (STR, "invention"), "INFO_TXT": (STR, "info_txt"),
        "INFO_FLC": (STR, "info_flc"), "UPGRADE": (STR, "upgrade"),
        "WEAPON": (STR, "weapon"),  # scalar here (unlike UNIT's slotted WEAPON)
        "FRAME": (STR, "frame"), "FRAME_2": (STR, "frame_2"),
        "EQUIVALENT": (STR, "equivalent"), "TYPE": (STR, "type"),
        "AIBLD": (STR, "aibld"), "ICON": (STR, "icon"),
        "SOUND_EXPLO": (STR, "sound_explo"), "SOUND_CLICK": (STR, "sound_click"),
        "COMPONENT_QUANT": (INT, "component_quant"),
        "SPRITE_QUANTITY": (INT, "sprite_quantity"), "HEIGHT": (INT, "height"),
        # PARAMETR feeds Building.unit_housing_capacity: per-building-TYPE unit
        # housing / storage-dock capacity. Dead in retail (Construct clobbers it
        # with 50); the storage_cap_variable exe patch revives it as the real
        # physical + authorization cap (set <=50 for storage buildings; 0 => 50).
        "PARAMETR": (INT, "housing_capacity"), "ENERGY": (INT, "energy"),
        "ELECTRIC_POWER": (INT, "electric_power"), "HUMAN": (INT, "human"),
        "BUILDER": (INT, "builder"), "SIGHT": (INT, "sight"),
        "TRACE": (INT, "trace"), "ABILITY": (INT, "ability"),
        "BUILD_TIME": (F32, "build_time"), "VELOCITY": (F32, "velocity"),
        "ANIM": (SLOT_STR, "anims"), "COMPONENT": (SLOT_STR, "components"),
        "ANIM_P": (SLOT_INT, "anim_p"), "AREA": (AREA, "area"),
        "RESOURCE": (RES, "resources"),
        "PRODUCTION": (SLOT_STR, "unit_name"), "TRANSPORT": (SLOT_STR, "unit_name"),
        "PRODUCTION_TIME": (SLOT_F32, "unit_quant"),
        "TRANSPORT_QUANT": (SLOT_F32, "unit_quant"),
        "CAPACITY": (CAP, "capacity"),
        "EXTRACT": (CAP, "extract_fuel"), "FUEL": (CAP, "extract_fuel"),
    },
    "PROGRESS": {
        "DEPEND": (DEPEND, "depends"), "TYPE": (STR, "type"),
    },
    "PLANET": {
        "MAP": (STR, "map"), "INVENTION": (STR, "invention"),
        "ICON": (STR, "icon"), "INFO_TXT": (STR, "info_txt"),
        "INFO_FLC": (STR, "info_flc"),
        "X1": (INT, "x1"), "X2": (INT, "x2"), "Y1": (INT, "y1"), "Y2": (INT, "y2"),
        "COORDINATE_X": (INT, "coordinate_x"), "COORDINATE_Y": (INT, "coordinate_y"),
        "ASTEROIDS": (INT, "asteroids"), "ENEMY": (INT, "enemy"),
        "TURN_SPEED": (INT, "turn_speed"),  # int here; f32 in UPGRADE
        "BANK": (BANK_RANGE, "banks"),
        "SOURCE_MUL": (SOURCE4, "source_mul"), "SOURCE_ADD": (SOURCE4, "source_add"),
    },
    "SYSTEM": {
        "PLANET": (SLOT_STR, "planets"),  # 1-based in retail; planets[0] unused
        "INVENTION": (STR, "invention"), "ICON": (STR, "icon"),
    },
    "UPGRADE": {
        "INVENTION": (STR, "invention"), "TYPE": (STR, "type"),
        "RANGE_MIN": (INT, "range_min"), "RANGE_MAX": (INT, "range_max"),
        "POWER": (INT, "power"), "MISSING": (INT, "missing"),
        "ENERGY": (INT, "energy"), "SIGHT": (INT, "sight"),
        "STEP_SPEED": (F32, "step_speed"), "TURN_SPEED": (F32, "turn_speed"),
        "OBJECT": (SLOT_STR, "objects"),
    },
    "PROJECT": {
        "INVENTION": (STR, "invention"), "ICON": (STR, "icon"),
        "TYPE": (STR, "type"), "INFO_TXT": (STR, "info_txt"),
        "INFO_FLC": (STR, "info_flc"), "BUILD_TIME": (F32, "build_time"),
        "RESOURCE": (RES, "resources"),
    },
    "STONE": {"SPRITE": (STR, "sprite")},
    "TREE": {"SPRITE": (STR, "sprite")},
    "BANK": {"SPRITE": (BANK_SPRITE, "sprites")},
}

# Non-zero / non-empty ObjectInit defaults (§6). Everything else is seeded to
# the kind's zero value by new_record.
DEFAULT_OVERLAY = {
    "ANIM": {"length": 1, "time": f32(0.1), "step": 1, "repeat": 1},
    "UNIT": {
        "sprite_type": "SPRITE_ZWYKLY", "soldier_type": "SOLDIERTYPE_UNKNOWN",
        "step_speed": f32(1.0), "turn_speed": f32(1.0), "energy": 10,
        "ai_level": 1, "sight": 5, "build_time": f32(1.0),
    },
    "BUILDING": {"height": 1},
    "WEAPON": {
        "ammo": 1, "pocket": 1, "short_time": f32(1.0), "long_time": f32(1.0),
        "range_min": 1, "range_max": 9, "power": 10, "speed": f32(1.0),
        "length": 200, "fire_range": 1, "smoke_time": f32(0.05),
        "probability": {0: 100},
    },
    "PLANET": {
        "x1": 10, "y1": 10, "x2": 40, "y2": 40, "enemy": 1,
        "source_mul": [1, 1, 1, 1], "source_add": [0, 0, 0, 0],
    },
}

_ZERO = {INT: 0, F32: f32(0.0), STR: "", FLAG: 0}


def _zero_for(kind):
    if kind in _ZERO:
        return _ZERO[kind]
    if kind in (SLOT_STR, SLOT_INT, SLOT_F32, CAP):
        return {}
    if kind in (RES, DEPEND):
        return []
    if kind == AREA:
        return [[0] * 10 for _ in range(10)]
    if kind == BANK_RANGE:
        return set()
    if kind == SOURCE4:
        return [0, 0, 0, 0]
    return None


def new_record(cls: str) -> dict:
    """Fresh staging record for *cls* with ObjectInit defaults applied."""
    rec = {}
    for kw, (kind, field) in CLASS_GRAMMAR[cls].items():
        if kind == BANK_SPRITE:
            continue
        if field not in rec:
            rec[field] = _zero_for(kind)
    import copy
    for field, val in DEFAULT_OVERLAY.get(cls, {}).items():
        rec[field] = copy.deepcopy(val)
    return rec


# ---------------------------------------------------------------------------
# Validation metadata (Phase 3). All caps/ref-universes below were measured
# against retail (the cfg grammar notes + structs.md); every ref field
# resolves 100% on retail, so dangling refs are hard errors.
# ---------------------------------------------------------------------------

NAME_MAXLEN = 127   # GetAsciiString dst is 127 chars + NUL
TEXT_MAXLEN = 119   # s_w_str_copy WCHAR cap for TEXT values

# Slotted/list caps: (cls, field) -> (mode, n).
#   "slot"  : 0 <= slot < n          "slot1" : 1 <= slot < n (0 reserved)
#   "count" : len(entries) <= n
# Sources: docs §7 rule 6 ([4]/[12]/[8]/[16]/[32]); struct array sizes
# (unit_quant[100]→20 per doc, capacity[10], extract[4]); RESOURCE/DEPEND buffers.
CAPS = {
    ("UNIT", "weapons"): ("slot", 4), ("UNIT", "resources"): ("count", 7),
    ("WEAPON", "bullet_anims"): ("slot", 4), ("WEAPON", "probability"): ("slot", 4),
    ("BUILDING", "anims"): ("slot", 12), ("BUILDING", "components"): ("slot", 8),
    ("BUILDING", "anim_p"): ("slot", 8), ("BUILDING", "unit_name"): ("slot", 20),
    ("BUILDING", "unit_quant"): ("slot", 20), ("BUILDING", "capacity"): ("slot", 10),
    ("BUILDING", "extract_fuel"): ("slot", 10), ("BUILDING", "resources"): ("count", 7),
    ("UPGRADE", "objects"): ("slot", 16),
    ("SYSTEM", "planets"): ("slot", 32),
    ("PROGRESS", "depends"): ("count", 16), ("PROJECT", "resources"): ("count", 4),
    ("*", "__string__"): ("bytes", 127), ("PLANET", "map"): ("bytes", 31),
    ("TEXT", "value"): ("utf16", 119), ("TEXT", "__pool__"): ("utf16", 150000),
    ("TEXT", "__scratch__"): ("utf16", 256),
    ("TEXT", "__info_scratch__"): ("utf16", 256), ("TEXT", "__info_fixed__"): ("utf16", 33),
    ("TEXT", "__resource_label__"): ("utf16", 19),
    ("ANIM", "__pool__"): ("frames", 2480),
    ("SPRITE", "__id__"): ("count", 22000),
    ("UNIT", "__count__"): ("count", 99), ("BUILDING", "__count__"): ("count", 99),
    ("WEAPON", "__count__"): ("count", 31), ("PROJECT", "__count__"): ("count", 99),
    ("UPGRADE", "__count__"): ("count", 99), ("PROGRESS", "__count__"): ("count", 299),
    ("SYSTEM", "__count__"): ("count", 3), ("PLANET", "__count__"): ("count", 30),
    ("TREE", "__count__"): ("count", 99), ("STONE", "__count__"): ("count", 99),
}
for _cls in ("UNIT", "BUILDING", "PLANET", "PROJECT"):
    for _field in ("info_txt", "info_flc"):
        CAPS[(_cls, _field)] = ("bytes", 63)

# Retail tables reserve index zero; CAPS count limits exclude that sentinel.
CLASS_CAPS = {cls: limit for (cls, field), (mode, limit) in CAPS.items()
              if field == "__count__" and mode == "count"}
ANIM_FRAME_BUDGET = CAPS[("ANIM", "__pool__")][1]

# Cross-reference fields: (cls, field) -> registry universe name (see
# validate.build_registry). Every entry measured 0-miss on retail.
REF = {
    ("ANIM", "sprite"): "sprite",
    ("UNIT", "sprite"): "sprite", ("UNIT", "sprite_shadow"): "sprite",
    ("UNIT", "frame"): "sprite", ("UNIT", "frame_2"): "sprite",
    ("UNIT", "anim_explo"): "anim", ("UNIT", "equivalent"): "building",
    ("UNIT", "weapons"): "weapon", ("UNIT", "sprite_type"): "define",
    ("BUILDING", "frame"): "sprite", ("BUILDING", "frame_2"): "sprite",
    ("BUILDING", "components"): "sprite", ("BUILDING", "anims"): "anim",
    ("BUILDING", "equivalent"): "unit", ("BUILDING", "upgrade"): "building",
    ("BUILDING", "weapon"): "weapon", ("BUILDING", "unit_name"): "unit",
    ("WEAPON", "fire_explo"): "anim", ("WEAPON", "target_explo"): "anim",
    ("WEAPON", "smoke_sprite"): "anim", ("WEAPON", "bullet_anims"): "anim",
    ("TREE", "sprite"): "sprite", ("STONE", "sprite"): "sprite",
    ("SYSTEM", "planets"): "planet", ("UPGRADE", "objects"): "upgrade_object",
    ("PROGRESS", "depends"): "progress",
}
for _cls in ("UNIT", "BUILDING", "PLANET", "UPGRADE", "PROJECT", "SYSTEM"):
    REF[(_cls, "invention")] = "progress"
for _cls in ("UNIT", "BUILDING", "PLANET", "PROJECT", "SYSTEM"):
    REF[(_cls, "icon")] = "icon"
for _cls, _fields in {
    "UNIT": ("type", "independent", "soldier_type", "aiunit", "sound_explo", "sound_move"),
    "BUILDING": ("type", "aibld", "sound_explo", "sound_click"),
    "WEAPON": ("type", "homing", "target", "sound_fire", "sound_target"),
    "PROGRESS": ("type",), "UPGRADE": ("type",), "PROJECT": ("type",),
}.items():
    for _field in _fields:
        REF[(_cls, _field)] = "define"
for _cls in ("UNIT", "BUILDING", "PROJECT"):
    REF[(_cls, "resources")] = "resource_define"
for _field in ("capacity", "extract_fuel"):
    REF[("BUILDING", _field)] = "resource_define"


# Asset-file fields (checked only with --assets-root; not a name registry).
ASSET_FIELDS = {("PLANET", "map"): ".MP"}

PROB_MAX = 100  # PROBABILYTY per-slot percentage

# exe string literals that must resolve to an anim/sprite (deletion = crash).
# Verified present in retail as ANIMs; S_RAMKA/A_SLAD are prefix families.
PROTECTED_NAMES = {
    "A_mozna_postawic", "A_nie_mozna_postawic",
    "A_OGIEN1", "A_OGIEN2", "A_OGIEN3", "A_OGIEN4",
    "A_DYM_POJAZD1", "A_DYM_POJAZD2", "A_DYM_POJAZD3", "A_DYM_POJAZD4",
}
PROTECTED_PREFIXES = ("S_RAMKA", "A_SLAD_")   # at least one name each must exist

TREE_BANK_RANGE = range(50, 57)   # each PLANET loads exactly one 50..56 tree bank
SPRITE_ID_CAP = 22000             # global sprite-id space bound (DEF_BANK arithmetic)

# Scalar limits directly proven by constructor/consumer access.
DOMAIN = {
    ("TEXT", "__id__"): (0, 805), ("TEXT", "__resource_label_id__"): (73, 76),
    ("TEXT", "__info_header_id__"): (736, 736),
    ("ANIM", "length"): (1, 2147483647), ("ANIM", "repeat"): (1, 2147483647),
    ("UNIT", "trace"): (0, 5), ("BUILDING", "trace"): (0, 5),
    ("PROJECT", "type"): (0, 1), ("UNIT", "type"): (1, 24), ("UNIT", "independent"): (0, 2),
    ("UNIT", "aiunit"): (0, 11), ("BUILDING", "component_quant"): (0, 7),
    ("BUILDING", "sprite_quantity"): (1, 11),
    ("UNIT", "resources"): (1, 4), ("BUILDING", "resources"): (1, 4),
    ("PROJECT", "resources"): (1, 9), ("BUILDING", "capacity"): (0, 9),
    ("BUILDING", "extract_fuel"): (0, 9),
    ("BUILDING", "housing_capacity"): (0, 50),
}

# Dispatcher modifier expressions: (class, canonical field, multiplier,
# addend, result requires signed int32 conversion). Retail quirks are explicit.
TRANSFORMS = [
    ("WEAPON", "power", "WEAPON_POWER_MUL", "WEAPON_POWER_ADD", True),
    ("UNIT", "sight", "UNIT_SIGHT_MUL", "UNIT_SIGHT_ADD", True),
    ("UNIT", "energy", "UNIT_ENERGY_MUL", "UNIT_ENERGY_ADD", True),
    ("UNIT", "resources", "UNIT_RESOURCE_MUL", "UNIT_RESOURCE_ADD", True),
    ("UNIT", "step_speed", "UNIT_SPEED_MUL", "UNIT_SPEED_ADD", False),
    ("UNIT", "turn_speed", "UNIT_SPEED_MUL", "UNIT_SPEED_ADD", False),
    ("UNIT", "build_time", "UNIT_BUILD_TIME_MUL", "UNIT_BUILD_TIME_MUL", False),
    ("BUILDING", "sight", "BUILDING_SIGHT_MUL", "BUILDING_SIGHT_ADD", True),
    ("BUILDING", "energy", "BUILDING_ENERGY_MUL", "BUILDING_ENERGY_ADD", True),
    ("BUILDING", "human", "BUILDING_HUMAN_MUL", "BUILDING_HUMAN_ADD", True),
    ("BUILDING", "builder", "BUILDING_BUILDER_MUL", "BUILDING_BUILDER_ADD", True),
    ("BUILDING", "build_time", "BUILDING_BUILD_TIME_MUL", "BUILDING_BUILD_TIME_ADD", False),
    ("BUILDING", "electric_power", "BUILDING_POWER_MUL", "BUILDING_ENERGY_ADD", True),
]

# Production-completion reason TEXT IDs and the first matching housing type
# (alien, human) for each reason. See sim_reason_to_housing_bldg.
HOUSING_REASONS = [(0, -1, -1), (15, 7, 27), (16, 8, 28), (17, 10, 30), (18, 9, 29)]

# BUILDING.TYPE constructor dispatch groups (resolved DEFINE values).
BUILDING_KINDS = [("mine", 2), ("mine", 22), ("port", 12), ("port", 32),
                  ("mother", 6), ("mother", 26), ("shuttle", 13), ("shuttle", 33), ("silo", 16), ("silo", 36)]
# Main bases and civilian records are not build-menu entries and use the
# zero-duration guarded online-toggle callback. Mothers can deploy and build.
BUILDING_KINDS += [("nonconstructible", value) for value in (14, 34, 18, 38)]
# Tick2's generic default and human airfield have unguarded time subtraction.
_TICK2_SPECIAL = {5, 6, 7, 8, 9, 10, 12, 13, 14, 18,
                  25, 26, 27, 28, 29, 30, 32, 33, 34, 38}
BUILDING_KINDS += [("tick_strict", value) for value in range(1, 40)
                   if value not in _TICK2_SPECIAL]
# Airfield and alien helipad callbacks seed sentinel holes; only the listed
# positions at or below SPRITE_QUANTITY can advance in their mapped paths.
BUILDING_KINDS += [(f"strict_count_slot_{slot}", 29) for slot in (1, 2, 4, 5, 6)]
BUILDING_KINDS += [(f"guarded_count_slot_{slot}", 10) for slot in (1, 3)]
# Explicit per-category cfg animation slots used by zero-guarded tick2 paths.
_GUARDED_ANIM_SLOTS = {
    7: (2, 3, 4, 5), 8: (2, 3, 4, 5), 27: (2, 3, 4, 5, 8),
    28: (3, 4, 5, 6), 30: (3, 4, 5, 6), 9: (3, 4, 5, 6),
    12: (2, 3, 4, 5, 6), 32: (2,), 13: (2, 3, 4, 5, 6, 10),
    33: (3, 4, 5, 6), 6: (1, 4, 5), 26: (1, 4, 5),
    14: (1,), 34: (1,), 18: (1,), 38: (1,),
}
BUILDING_KINDS += [(f"guarded_slot_{slot}", kind)
                   for kind, slots in _GUARDED_ANIM_SLOTS.items() for slot in slots]

