"""Synthetic-data test for the wiki model (no retail data needed).

Run either way::

    python src/formats/wiki/test_model.py
    python -m pytest src/formats/wiki/test_model.py        # if pytest is installed

A tiny INIT.CFG / INITLANG.CFG (EN + RU) / INFO.TXT is written to a temp dir, loaded through
``source.load_files`` and checked against every in-game formula, the two retail bugs, round half up,
the language fallbacks, slugs, links and the invention graph.
"""

import os
import sys
import tempfile

_REPO = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)
if _REPO not in sys.path:
    sys.path.insert(0, _REPO)

from src.formats.wiki import model, source

INIT = r"""
UNIT_ENERGY_MUL 1.5
UNIT_SIGHT_ADD 2
UNIT_BUILD_TIME_MUL 2
WEAPON_POWER_ADD 1
BUILDING_ENERGY_MUL 2.5
BUILDING_ENERGY_ADD 3
BUILDING_POWER_MUL 2
BUILDING_SIGHT_ADD 4
BUILDING_BUILD_TIME_MUL 0.4

DEFINE "R_hZALOGA3" 8
DEFINE "R_hZALOGA1" 6
DEFINE "R_hPOJAZD" 13
DEFINE "R_aSAMOLOT" 17
DEFINE "R_hFABRYKA" 21
DEFINE "R_aELEKTROWNIA" 3
DEFINE "R_aOSIEDLE" 4
DEFINE "R_hMATKA" 26
DEFINE "R_hPORT" 32
DEFINE "R_hSTATEK" 33
DEFINE "R_hKOPALNIA" 22
DEFINE "R_hBAZA" 34
DEFINE "R_hCHATKA" 38
DEFINE "ZIEMIA_POWIETRZE" 3
DEFINE "ZIEMIA" 1
DEFINE "zloze_a" 1
DEFINE "zloze_b" 2
DEFINE "zloze_c" 3
DEFINE "zloze_d" 4
ICON "I_t" 7

WEAPON "BRON_T"
   TARGET "ZIEMIA_POWIETRZE"
   RANGE_MIN 1
   RANGE_MAX 4
   POWER 10
END
WEAPON "BRON_G"
   TARGET "ZIEMIA"
   POWER 5
END

UNIT "N_human_Soldier3"
   INVENTION "V_human_s3"
   TYPE "R_hZALOGA3"
   SOLDIER_TYPE "SOLDIERTYPE_SOLDIER"
   ICON "I_t"
   INFO_TXT "ludz_p\HP_S3.INF"
   ENERGY 60
   SIGHT 4
   STEP_SPEED 0.032
   BUILD_TIME 1.0
   RESOURCE 300 "zloze_a"
   RESOURCE 180 "zloze_b"
   WEAPON 0 "BRON_T"
END
UNIT "N_human_Soldier1"
   INVENTION "V_human_s1"
   TYPE "R_hZALOGA1"
   ENERGY 3
   SIGHT 0
   STEP_SPEED 0.032
END
UNIT "N_human_Tank"
   INVENTION "V_human_tank"
   TYPE "R_hPOJAZD"
   ENERGY 500
   SIGHT 5
   STEP_SPEED 0.025
   BUILD_TIME 30.0
   WEAPON 0 "BRON_T"
   WEAPON 1 "BRON_G"
   ARMOR 4
   INFO_TXT "ludz_p\HP_TANK.INF"
END
UNIT "N_alien_Jet"
   INVENTION "V_alien_jet"
   TYPE "R_aSAMOLOT"
   ENERGY 100
   STEP_SPEED 0.008
END

BUILDING "N_alien_Reactor"
   INVENTION "V_alien_reactor"
   TYPE "R_aELEKTROWNIA"
   ENERGY 800
   ELECTRIC_POWER 1200
   HUMAN 50
   BUILD_TIME 30.0
   SIGHT 4
   RESOURCE 700 "zloze_a"
   RESOURCE 500 "zloze_b"
   RESOURCE 300 "zloze_c"
   RESOURCE 120 "zloze_d"
END
BUILDING "N_alien_Colony"
   INVENTION "V_alien_colony"
   TYPE "R_aOSIEDLE"
   ENERGY 500
   ELECTRIC_POWER 200
   HUMAN 100
   BUILD_TIME 25.0
END
BUILDING "N_human_Factory"
   INVENTION "V_human_factory"
   TYPE "R_hFABRYKA"
   ENERGY 1000
   ELECTRIC_POWER 100
   BUILD_TIME 90.0
   UPGRADE "N_human_Factory2"
   PRODUCTION 0 "N_human_Tank"
   PRODUCTION 1 "N_human_Soldier3"
   INFO_TXT "ludz_b\HB_F.INF"
END
BUILDING "N_human_Factory2"
   INVENTION "V_human_factory2"
   TYPE "R_hFABRYKA"
   ENERGY 1500
   ELECTRIC_POWER 100
   BUILD_TIME 100.0
   INFO_TXT "ludz_b\HB_F2.INF"
   PRODUCTION 0 "N_human_Tank"
   UPGRADE "N_human_Factory3"
END
BUILDING "N_human_Factory3"
   INVENTION "V_human_factory3"
   TYPE "R_hFABRYKA"
   ENERGY 1800
   ELECTRIC_POWER 100
   BUILD_TIME 110.0
END
BUILDING "N_human_Baza"
   INVENTION "V_human_baza"
   TYPE "R_hBAZA"
   ENERGY 999999
END
BUILDING "N_human_Chatka"
   INVENTION "V_human_chatka1"
   TYPE "R_hCHATKA"
END
BUILDING "N_human_Chatka"
   INVENTION "V_human_chatka2"
   TYPE "R_hCHATKA"
END
BUILDING "N_human_Mother"
   INVENTION "V_human_mother"
   TYPE "R_hMATKA"
   ENERGY 2000
   ELECTRIC_POWER 400
   HUMAN 300
   VELOCITY 0.15
   BUILD_TIME 180.0
   EXTRACT 0 "zloze_a" 1500.0
   FUEL 1 "zloze_b" 1500.4
   RESOURCE 3000 "zloze_a"
END
BUILDING "N_human_Port"
   INVENTION "V_human_port"
   TYPE "R_hPORT"
   ENERGY 800
   ELECTRIC_POWER 200
   VELOCITY 0.7
   BUILD_TIME 90.0
END
BUILDING "N_human_Mine"
   INVENTION "V_human_mine"
   TYPE "R_hKOPALNIA"
   ENERGY 1000
   EXTRACT 0 "zloze_a" 200.5
   EXTRACT 1 "zloze_b" 150.0
END
BUILDING "N_x_Hut"
   INVENTION "V_x_hut1"
   TYPE "R_hFABRYKA"
END
BUILDING "N_x_Hut"
   INVENTION "V_x_hut2"
   TYPE "R_hFABRYKA"
END

PROJECT "N_human_pr_Factory2"
   INVENTION "V_human_pr_factory2"
   INFO_TXT ""
   BUILD_TIME 20.0
   RESOURCE 100 "zloze_a"
END
PROJECT "N_human_pr_Tank"
   INVENTION "V_human_pr_tank"
   INFO_TXT "ludz_pr\LU_TANK.INF"
   BUILD_TIME 34.0
END
PROJECT "N_human_pr_Armour"
   INVENTION "V_human_pr_armour"
   INFO_TXT "ludz_pr\LU_ARM.INF"
   BUILD_TIME 10.0
END
PROJECT "N_human_pr_Gun"
   INVENTION "V_human_pr_gun"
   INFO_TXT "ludz_pr\LU_GUN.INF"
   BUILD_TIME 10.0
END

UPGRADE "N_human_up_Armour"
   INVENTION "V_human_up_armour"
   TYPE "unit"
   ENERGY 25
   STEP_SPEED 20.0
   OBJECT 1 "N_human_Soldier3"
   OBJECT 2 "N_human_Tank"
END
UPGRADE "N_human_up_Gun"
   INVENTION "V_human_up_gun"
   TYPE "weapon"
   POWER 35
   RANGE_MAX -2
   OBJECT 1 "BRON_T"
END

PLANET "N_planeta_1"
   INVENTION "V_planeta_1"
   MAP "p1.mp"
   INFO_FLC "BLUE1.FLC"
   COORDINATE_X 100
   COORDINATE_Y 200
END
PLANET "N_planeta_2"
   INVENTION "V_planeta_2"
   COORDINATE_X 300
   COORDINATE_Y 400
END
PLANET "N_planeta_3"
   INVENTION "V_planeta_3"
   COORDINATE_X 10
   COORDINATE_Y 10
END
PLANET "N_planeta_4"
   INVENTION "V_planeta_4"
   COORDINATE_X 20
   COORDINATE_Y 10
END
SYSTEM "N_uklad1"
   INVENTION "V_uklad1"
   PLANET 1 "N_planeta_1"
   PLANET 2 "N_planeta_2"
END
SYSTEM "N_uklad2"
   INVENTION "V_uklad2"
   PLANET 1 "N_planeta_3"
   PLANET 2 "N_planeta_4"
END

PROGRESS "V_planeta_1" TYPE "planeta" END
PROGRESS "V_planeta_2" TYPE "planeta" DEPEND "V_planeta_1" END
PROGRESS "V_uklad1" TYPE "uklad" END
PROGRESS "V_planeta_3" TYPE "planeta" END
PROGRESS "V_planeta_4" TYPE "planeta" DEPEND "V_planeta_3" END
PROGRESS "V_uklad2" TYPE "uklad" DEPEND "V_uklad1" END
PROGRESS "V_human_factory3" TYPE "budynek" DEPEND "V_human_factory2" END
PROGRESS "V_human_baza" TYPE "budynek" DEPEND "V_human_mother" END
PROGRESS "V_human_chatka1" TYPE "budynek" DEPEND "V_human_mother" END
PROGRESS "V_human_chatka2" TYPE "budynek" DEPEND "V_human_mother" END
PROGRESS "V_human_mother" TYPE "budynek" END
PROGRESS "V_human_factory" TYPE "budynek" DEPEND "V_human_mother" END
PROGRESS "V_human_pr_factory2" TYPE "projekt" DEPEND "V_human_factory" DEPEND "V_planeta_1" END
PROGRESS "V_human_factory2" TYPE "budynek" DEPEND "V_human_factory" DEPEND "V_human_pr_factory2" END
PROGRESS "V_human_pr_tank" TYPE "projekt" DEPEND "V_human_factory" END
PROGRESS "V_human_tank" TYPE "pojazd" DEPEND "V_human_factory" DEPEND "V_human_pr_tank" END
PROGRESS "V_human_s3" TYPE "pojazd" DEPEND "V_human_mother" END
PROGRESS "V_human_s1" TYPE "pojazd" DEPEND "V_human_mother" END
PROGRESS "V_human_port" TYPE "budynek" DEPEND "V_human_mother" END
PROGRESS "V_human_mine" TYPE "budynek" DEPEND "V_human_mother" END
PROGRESS "V_human_pr_armour" TYPE "projekt" DEPEND "V_human_mother" END
PROGRESS "V_human_up_armour" TYPE "upgrade" DEPEND "V_human_pr_armour" END
PROGRESS "V_human_pr_gun" TYPE "projekt" DEPEND "V_human_mother" END
PROGRESS "V_human_up_gun" TYPE "upgrade" DEPEND "V_human_pr_gun" END
PROGRESS "V_alien_reactor" TYPE "budynek" DEPEND "V_planeta_2" END
PROGRESS "V_alien_colony" TYPE "budynek" DEPEND "V_alien_reactor" END
PROGRESS "V_alien_jet" TYPE "pojazd" DEPEND "V_alien_colony" END
PROGRESS "V_x_hut1" TYPE "budynek" END
PROGRESS "V_x_hut2" TYPE "budynek" END
"""

LANG_EN = r"""
TEXT "N_human_Soldier3" "3 Soldiers"
TEXT "N_human_Soldier1" "Soldier"
TEXT "N_human_Tank" "Tank"
TEXT "N_alien_Jet" "Jet"
TEXT "N_alien_Reactor" "Reactor"
TEXT "N_alien_Colony" "Hive"
TEXT "N_human_Factory" "Factory"
TEXT "N_human_Factory2" "Heavy Factory"
TEXT "N_human_Mother" "Pioneer"
TEXT "N_human_Port" "Port"
TEXT "N_human_Mine" "Mine"
TEXT "N_x_Hut" "Hut"
TEXT "N_human_pr_Factory2" "Heavy Factory Upgrade"
TEXT "N_human_pr_Tank" "Tank Project"
TEXT "N_human_pr_Armour" "Armour Plating"
TEXT "N_human_pr_Gun" "Gun Project"
TEXT "BRON_T" "Test Gun"
TEXT "BRON_G" "Ground Gun"
TEXT "N_planeta_1" "Alpha"
TEXT "N_planeta_2" "Beta"
TEXT "N_uklad1" "First System"
TEXT "N_uklad2" "Second System"
TEXT "N_human_Factory3" "Mega Factory"
TEXT "N_human_Baza" "Base"
TEXT "N_human_Chatka" "Cottage"
TEXT "K_MENU_Speed" "Speed:"
TEXT "K_MENU_Energy" "Energy:"
TEXT "K_MENU_Sight" "Sight range:"
TEXT "K_MENU_MaxPeople" "Max people:"
TEXT "K_MENU_MaxPeople_a" "Max drones:"
TEXT "K_MENU_GA" "GA"
TEXT "K_MENU_Speed_Unit" "km/h"
"""

LANG_RU = r"""
TEXT "N_human_Soldier3" "3 Солдата"
TEXT "N_human_Tank" "Танк"
TEXT "N_alien_Reactor" "Реактор"
TEXT "BRON_T" "Тестовая пушка"
TEXT "K_MENU_Speed" "Скорость:"
TEXT "K_MENU_GA" "НВ"
"""

INFO_EN = """<FILE ludz_p\\HP_S3.INF>
Three soldiers.

<FILE ludz_p\\HP_TANK.INF>
A tank.
Second line.
<FILE ludz_b\\HB_F.INF>
A factory.

<FILE ludz_b\\HB_F2.INF>
A heavy factory.

<FILE ludz_pr\\LU_TANK.INF>
Tank research.
"""

INFO_RU = """<FILE ludz_p\\HP_S3.INF>
Три солдата.

<FILE ludz_b\\HB_F2.INF>
Тяжелый завод.
"""


def _w(path, text):
    with open(path, "wb") as f:
        f.write(("﻿" + text.replace("\n", "\r\n")).encode("utf-16-le"))


def build_wiki():
    d = tempfile.mkdtemp(prefix="wiki_test_")
    _w(os.path.join(d, "INIT.CFG"), INIT)
    for lang, lt, it in (("en", LANG_EN, INFO_EN), ("ru", LANG_RU, INFO_RU)):
        _w(os.path.join(d, f"initlang_{lang}.cfg"), lt)
        _w(os.path.join(d, f"info_{lang}.txt"), it)
    data = source.load_files(
        os.path.join(d, "INIT.CFG"),
        {
            "ru": (os.path.join(d, "initlang_ru.cfg"), os.path.join(d, "info_ru.txt")),
            "en": (os.path.join(d, "initlang_en.cfg"), os.path.join(d, "info_en.txt")),
        },
    )
    return model.build(data)


W = build_wiki()


def row(entity, label_key, side="left"):
    for r in getattr(entity.panel, side):
        if r.label_key in (label_key, label_key + "_a"):
            return r
    return None


def u(name):
    return W.by_cfg("unit", name)


def b(name):
    return W.by_cfg("building", name)


# ---------------------------------------------------------------------------------- tests


def test_counts_and_languages():
    assert W.langs == ["en", "ru"]
    c = W.counts()
    assert c["unit"] == 4 and c["building"] == 13 and c["project"] == 4, c
    assert c["weapon"] == 2 and c["planet"] == 4 and c["system"] == 2 and c["upgrade"] == 2, c


def test_round_half_up():
    assert model.rnd(0.5) == 1 and model.rnd(2.5) == 3 and model.rnd(4.5) == 5
    assert model.rnd(1.4999) == 1 and model.rnd(-0.2) == 0
    # Soldier1: energy 3 * 1.5 = 4.5 -> 5 (banker's rounding would give 4)
    assert row(u("N_human_Soldier1"), "K_MENU_Energy").value == 5


def test_unit_formulas():
    s3 = u("N_human_Soldier3")
    assert row(s3, "K_MENU_Speed").value == 25  # 0.8 / 0.032
    assert row(s3, "K_MENU_Speed").unit_key == "K_MENU_Speed_Unit"
    assert row(s3, "K_MENU_Energy").value == 90  # 60 * 1.5
    assert row(s3, "K_MENU_Sight").value == 300  # (4 + 2) * 50
    assert row(s3, "K_MENU_Sight").unit_key == "K_MENU_Sight_Unit"
    # retail bug: build_time * MUL + MUL = 1*2 + 2 (not 1*2 + ADD(0))
    assert row(s3, "K_MENU_ProdTime").value == 4
    assert row(u("N_human_Tank"), "K_MENU_ProdTime").value == 62
    assert row(u("N_human_Tank"), "K_MENU_Speed").value == 64  # 1.6 / 0.025 (ground)
    assert row(u("N_alien_Jet"), "K_MENU_Speed").value == 1200  # 9.6 / 0.008 (plane)
    assert [c.value for c in s3.panel.costs] == [300, 180, 0, 0]
    assert s3.panel.costs_label_key == "K_MENU_Costs"
    assert s3.soldier_count == 3 and u("N_human_Tank").soldier_count == 0


def test_energy_is_hp_note():
    assert "HP" in row(u("N_human_Tank"), "K_MENU_Energy").note


def test_weapon_rows():
    s3 = u("N_human_Soldier3")
    wr = s3.panel.weapons[0]
    assert (wr.power, wr.range_min, wr.range_max, wr.target_mask) == (
        11,
        1,
        4,
        3,
    )  # 10 + WEAPON_POWER_ADD 1
    assert W.get("weapon", wr.weapon.slug).names["en"] == "Test Gun"
    assert (
        W.target_letters("en", 3) == "GA"
        and W.target_letters("en", 1) == "G"
        and W.target_letters("en", 2) == "A"
    )
    assert W.target_letters("ru", 3) == "НВ"  # per-language letters from K_MENU_GA
    tank = u("N_human_Tank")
    assert [w.slot for w in tank.panel.weapons] == [0, 1] and tank.panel.weapons[1].target_mask == 1


def test_building_formulas_and_bugs():
    reactor = b("N_alien_Reactor")
    # retail bug: (electric_power + BUILDING_ENERGY_ADD) * BUILDING_POWER_MUL = (1200 + 3) * 2
    assert row(reactor, "K_MENU_EnergyProd").value == 2406
    assert row(reactor, "K_MENU_Energy").value == 2003  # 800 * 2.5 + BUILDING_ENERGY_ADD 3
    assert row(reactor, "K_MENU_BuildTime").value == 12  # 30 * 0.4
    assert row(reactor, "K_MENU_BuildTime").verified is False  # UNVERIFIED item flagged
    assert [c.value for c in reactor.panel.costs] == [700, 500, 300, 120]
    assert row(b("N_human_Factory"), "K_MENU_EnergyReq") is not None  # non-plant -> "required"
    assert row(b("N_human_Factory"), "K_MENU_BuildTime").value == 36


def test_people_and_mother():
    colony = b("N_alien_Colony")
    assert row(colony, "K_MENU_MaxPeople").label_key == "K_MENU_MaxPeople_a"  # alien variant
    assert row(colony, "K_MENU_MaxPeople").value == 100
    mother = b("N_human_Mother")
    assert row(mother, "K_MENU_MaxPeople").label_key == "K_MENU_MaxPeople"
    assert row(mother, "K_MENU_BuildTime") is None  # not shown for mothers
    assert mother.panel.costs == [] and mother.panel.costs_label_key is None  # no costs line
    assert row(mother, "K_MENU_EnergyProd") is not None  # mother counts as a power source row
    assert row(mother, "K_MENU_FlightSpeed").value == 6667  # 1000 / 0.15
    assert row(b("N_human_Port"), "K_MENU_FlightSpeed").value == 1429  # 1000 / 0.7
    assert row(b("N_alien_Colony"), "K_MENU_MaxWorkers") is None


def test_right_column():
    fac = b("N_human_Factory")
    r = row(fac, "K_MENU_UnitsTrained", "right")  # a slot holds soldiers -> "trained"
    assert r is not None and [i.link.slug for i in r.items] == ["human_tank", "human_soldier3"]
    assert row(b("N_human_Factory2"), "K_MENU_UnitsProduced", "right") is not None
    mine = row(b("N_human_Mine"), "K_MENU_ResMined", "right")
    assert [(i.label_key, i.value) for i in mine.items] == [
        ("K_CONS_RESOURCE", 201),
        ("K_CONS_RESOURCE1", 150),
    ]
    fuel = row(b("N_human_Mother"), "K_MENU_FlightCosts", "right")
    assert [i.value for i in fuel.items] == [
        1500,
        1500,
    ]  # EXTRACT/FUEL share one array; 1500.4 rounds


def test_building_upgrade_block():
    up = b("N_human_Factory").panel.upgrade
    assert up["target"] == model.Link("building", "human_factory2", "")
    assert up["project"].slug == "human_pr_factory2"  # "(Research required)" links the project
    assert up["panel"] is b("N_human_Factory2").panel
    assert b("N_human_Factory2").links["upgrade_from"][0].slug == "human_factory"


def test_building_chains():
    f1, f2, f3 = b("N_human_Factory"), b("N_human_Factory2"), b("N_human_Factory3")
    assert [lk.slug for lk in f1.chain] == ["human_factory", "human_factory2", "human_factory3"]
    assert f1.chain == f2.chain == f3.chain  # one chain list on every member
    assert (f1.level, f2.level, f3.level) == (0, 1, 2)
    assert f3.chain_base.slug == "human_factory"
    assert [x.slug for x in W.chain_members(f2)] == [
        "human_factory",
        "human_factory2",
        "human_factory3",
    ]
    assert b("N_human_Port").chain == [model.Link("building", "human_port", "")]
    listed = {x.slug for x in W.listed("building")}
    assert "human_factory" in listed and not {"human_factory2", "human_factory3"} & listed
    assert len(W.listed("building")) == len(W.buildings) - 2
    assert W.listed("unit") == W.units


def test_unbuildable_buildings():
    for n in ("N_human_Baza", "N_human_Chatka"):
        assert not b(n).buildable and b(n).unbuildable_why
    assert [x.slug for x in W.buildings if x.cfg_name == "N_human_Chatka"] == [
        "human_chatka",
        "human_chatka_2",
    ]
    assert all(x.buildable for x in W.buildings if x.role_name in ("factory", "port", "mother"))
    assert b("N_human_Baza") in W.listed("building")  # the pages stay


def test_starmap_layout():
    lay = W.starmap_layout(W.by_cfg("system", "N_uklad1"))
    assert [p["slug"] for p in lay] == ["planeta_1", "planeta_2"]
    # bounding box (100..300, 200..400) fills (1024-128) x (768-210), plus the 32 px pad
    assert (lay[0]["x"], lay[0]["y"]) == (32, 32)
    assert (lay[1]["x"], lay[1]["y"]) == (32 + 896, 32 + 558)
    lay2 = W.starmap_layout(W.by_cfg("system", "N_uklad2"))  # equal y: no division by zero
    assert (lay2[0]["x"], lay2[0]["y"], lay2[1]["x"]) == (32, 32, 928)


def test_project_panel():
    pr = W.by_cfg("project", "N_human_pr_Factory2")
    assert pr.panel.offers[0].role == "building_upgrade"
    assert pr.panel.offers[0].label_key == "K_MENU_BuildUpg"
    assert pr.panel.research.value == 20 and pr.panel.research.shown is False
    assert "wiki" in pr.panel.research.note
    assert [c.value for c in pr.panel.costs] == [100, 0, 0, 0]
    # empty INFO_TXT -> description of the first unlocked building
    assert pr.description["en"] == "A heavy factory."
    tank = W.by_cfg("project", "N_human_pr_Tank")
    assert tank.panel.offers[0].role == "unit" and tank.panel.offers[0].link.slug == "human_tank"
    arm = W.by_cfg("project", "N_human_pr_Armour")
    off = arm.panel.offers[0]
    assert off.role == "upgrade_unit"
    # affected units skip soldier_count >= 2 (Soldier3 is skipped, Tank stays)
    assert [a.slug for a in off.affected] == ["human_tank"]
    rows = {r.label_key: r for r in off.panel.left}
    assert rows["K_MENU_Speed"].value == 20 and rows["K_MENU_Speed"].marker == "+"
    assert rows["K_MENU_Armor"].value == 25 and rows["K_MENU_Armor"].marker == "+"
    gun = W.by_cfg("project", "N_human_pr_Gun").panel.offers[0]
    assert gun.role == "upgrade_weapon" and [a.slug for a in gun.affected] == ["bron_t"]
    g = {r.label_key: r for r in gun.panel.left}
    assert g["K_MENU_Power"].value == 35 and g["K_MENU_Power"].marker == "+"
    assert g["K_MENU_RangeMax"].value == 2 and g["K_MENU_RangeMax"].marker == "-"
    assert "K_MENU_RangeMin" not in g  # zero values are hidden
    assert all(not r.verified for r in gun.panel.left)  # UNVERIFIED semantics flagged
    # upgrades are named after their research project
    assert W.by_cfg("upgrade", "N_human_up_Armour").names["en"] == "Armour Plating"


def test_language_fallbacks():
    assert u("N_human_Soldier3").names == {"en": "3 Soldiers", "ru": "3 Солдата"}
    assert u("N_human_Tank").names["ru"] == "Танк"
    jet = u("N_alien_Jet")
    assert jet.names == {"en": "Jet", "ru": "Jet"}  # RU missing -> EN text
    assert "N_alien_Jet" in W.missing["ru"] and "N_alien_Jet" not in W.missing["en"]
    assert "K_MENU_Energy" in W.missing["ru"]
    assert (
        W.label("ru", "K_MENU_Speed") == "Скорость:" and W.label("ru", "K_MENU_Energy") == "Energy:"
    )
    s3 = u("N_human_Soldier3")
    assert s3.description["en"] == "Three soldiers." and s3.description["ru"] == "Три солдата."
    tank = u("N_human_Tank")
    assert tank.description["en"] == "A tank.\nSecond line."
    assert tank.description["ru"] == "A tank.\nSecond line." and tank.description_fallback == ["ru"]


def test_extra_fields_marked():
    tank = u("N_human_Tank")
    armor = next(r for r in tank.extra if r.cfg_key == "ARMOR")
    assert armor.value == 4 and armor.shown is False and armor.note == "config, not shown in game"
    assert all(r.shown is False for r in tank.extra)


def test_slugs_and_factions():
    assert [e.slug for e in W.buildings if e.cfg_name == "N_x_Hut"] == ["x_hut", "x_hut_2"]
    assert u("N_human_Soldier3").slug == "human_soldier3"
    assert u("N_human_Soldier3").faction == "human" and u("N_alien_Jet").faction == "alien"
    assert W.by_cfg("planet", "N_planeta_1").faction is None
    assert len({e.slug for e in W.buildings}) == len(W.buildings)
    assert W.get("weapon", "bron_t").faction == "human"  # derived from its users


def test_links():
    tank = u("N_human_Tank")
    assert [lk.slug for lk in tank.links["weapons"]] == ["bron_t", "bron_g"]
    assert sorted(lk.slug for lk in tank.links["produced_by"]) == [
        "human_factory",
        "human_factory2",
    ]
    assert [lk.slug for lk in tank.links["research"]] == ["human_pr_tank"]
    assert [lk.slug for lk in tank.links["upgrades"]] == ["human_up_armour"]
    # prerequisites (AND) and the inverted edges
    assert {lk.slug for lk in tank.links["requires"]} == {"human_factory", "human_pr_tank"}
    pr = W.by_cfg("project", "N_human_pr_Tank")
    assert [lk.slug for lk in pr.links["unlocks"]] == ["human_tank"]
    assert {lk.slug for lk in b("N_human_Mother").links["unlocks"]} >= {
        "human_factory",
        "human_port",
    }
    bt = W.get("weapon", "bron_t")
    assert {lk.slug for lk in bt.links["used_by_units"]} == {"human_soldier3", "human_tank"}
    assert [lk.slug for lk in bt.links["upgrades"]] == ["human_up_gun"]
    pl = W.by_cfg("planet", "N_planeta_2")
    assert pl.links["system"][0].slug == "uklad1" and pl.links["requires"][0].slug == "planeta_1"
    assert {lk.slug for lk in W.by_cfg("planet", "N_planeta_1").links["unlocks"]} >= {
        "planeta_2",
        "human_pr_factory2",
    }
    assert [lk.slug for lk in W.by_cfg("system", "N_uklad1").links["planets"]] == [
        "planeta_1",
        "planeta_2",
    ]


def test_tech_graph():
    g = W.tech_graph()
    ids = [n["id"] for n in g["nodes"]]
    assert len(ids) == len(set(ids)) == len(W.inventions)
    assert set(ids) == set(W.inventions)
    assert all(s in ids and d in ids for s, d in g["edges"])
    assert ("V_human_factory", "V_human_tank") in g["edges"] and (
        "V_human_pr_tank",
        "V_human_tank",
    ) in g["edges"]
    assert g == W.tech_graph()  # deterministic
    alien = W.tech_graph("alien")
    aids = {n["id"] for n in alien["nodes"]}
    assert "V_alien_jet" in aids and "V_human_tank" not in aids
    assert {"V_planeta_1", "V_planeta_2"} <= aids  # neutral prerequisites, transitively
    assert "V_uklad1" not in aids and "V_planeta_1" in aids
    assert all(s in aids and d in aids for s, d in alien["edges"])
    node = {n["id"]: n for n in g["nodes"]}["V_human_up_armour"]
    assert node["page"].slug == "human_pr_armour" and node["names"]["en"] == "Armour Plating"


def test_source_parse_info():
    info = source.parse_info("<FILE A\\B.INF>\r\nOne\r\n\r\n<FILE c\\d.inf>\r\nTwo\r\nlines\r\n")
    assert info == {"a\\b.inf": "One", "c\\d.inf": "Two\nlines"}


def main():
    names = sorted(n for n in globals() if n.startswith("test_"))
    for n in names:
        globals()[n]()
        print("ok", n)
    print(f"{len(names)} tests passed")


if __name__ == "__main__":
    main()
