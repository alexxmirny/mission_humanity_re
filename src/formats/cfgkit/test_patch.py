"""Behavioral tests for the cfgkit patch oracle."""
from __future__ import annotations

import copy
import unittest

from . import grammar as G, parse
from .patch import Input, PatchError, merge


def layer(index, path, text):
    return Input(f"mod{index}:{path}", path, text.encode("utf-8"), index)


def fixture():
    m = parse.Model()
    m.defines = [("ore", 1), ("title", 10), ("other", 11), ("project", 0), ("unit_type", 1)]
    m.defines += [(f"label{n}", n) for n in range(73, 77)]
    m.texts = [("title", "base"), ("other", "other base")]
    m.texts += [(f"label{n}", "ore") for n in range(73, 77)]
    for name in ("one", "two", "two"):
        r = G.new_record("UNIT")
        r["__name__"] = name
        # Avoid default symbolic refs for this independent synthetic fixture.
        r["type"] = "unit_type"
        r["sprite_type"] = ""
        r["soldier_type"] = ""
        r["resources"] = [("ore", 7)]
        m.classes["UNIT"].append(r)
    return m


class PatchTests(unittest.TestCase):
    def test_priority_group_clear_and_defaults(self):
        m = fixture()
        a = layer(0, "cfg/patch.cfg", '@patch UNIT "one"\nRESOURCE 3 "ore"\nRESOURCE 4 "ore"\nENERGY 20\nEND')
        b = layer(1, "cfg/patch.cfg", '@patch UNIT "one"\n@clear RESOURCE\nENERGY 30\nEND')
        result = merge(m, [b, a], "en")
        self.assertEqual(result.classes["UNIT"][0]["energy"], 30)
        self.assertEqual(result.classes["UNIT"][0]["resources"], [])
        self.assertEqual(m.classes["UNIT"][0]["resources"], [("ore", 7)])
        c = layer(2, "cfg/patch.cfg", '@replace UNIT "one"\nTYPE "unit_type"\nEND')
        # Replacement exposes its true default references; provide them in DEFINE.
        m.defines += [("SPRITE_ZWYKLY", 0), ("SOLDIERTYPE_UNKNOWN", 0)]
        self.assertEqual(merge(m, [c], "en").classes["UNIT"][0]["energy"], 10)

    def test_duplicates_and_unknown(self):
        m = fixture()
        for selector in ('UNIT "two"', 'UNIT "two" INDEX 1', 'UNIT "absent"'):
            with self.subTest(selector=selector), self.assertRaises(PatchError):
                merge(m, [layer(0, "cfg/patch.cfg", f'@patch {selector}\nEND')], "en")
        out = merge(m, [layer(0, "cfg/patch.cfg", '@patch UNIT "two" INDEX 3\nENERGY 42\nEND')], "en")
        self.assertEqual([r["energy"] for r in out.classes["UNIT"]], [10, 10, 42])

    def test_later_layer_repairs_earlier_candidate(self):
        m = fixture()
        a = layer(0, "cfg/patch.cfg", '@patch UNIT "one" WEAPON 0 "missing" END')
        b = layer(1, "cfg/patch.cfg", '@patch UNIT "one" @clear WEAPON END')
        self.assertEqual(merge(m, [a, b], "en").classes["UNIT"][0]["weapons"], {})

    def test_invalid_later_is_nonmutating(self):
        m = fixture()
        before = copy.deepcopy(m.__dict__)
        a = layer(0, "cfg/patch.cfg", '@patch UNIT "one"\nENERGY 30\nEND')
        b = layer(1, "cfg/patch.cfg", '@patch UNIT "one"\nWEAPON 0 "missing"\nEND')
        with self.assertRaises(PatchError):
            merge(m, [a, b], "en")
        self.assertEqual(m.__dict__, before)

    def test_per_key_fallback_and_all_languages_validated(self):
        m = fixture()
        en = layer(0, "lang/en.cfg", 'TEXT "title" "english"\nTEXT "other" "fallback"')
        ru = layer(0, "lang/ru.cfg", 'TEXT "title" "русский"')
        result = merge(m, [ru, en], "ru")
        self.assertEqual(result.texts[:2], [("title", "русский"), ("other", "fallback")])
        bad = layer(0, "lang/de.cfg", 'TEXT "absent" "bad"')
        with self.assertRaises(PatchError):
            merge(m, [en, ru, bad], "ru")

    def test_utf16_limit_and_utf8(self):
        m = fixture()
        valid = "😀" * 59 + "x"  # 119 UTF-16 code units
        out = merge(m, [layer(0, "lang/en.cfg", f'TEXT "title" "{valid}"')], "en")
        self.assertEqual(out.texts[0], ("title", valid))
        for value in ("x" * 120, "😀" * 60):
            with self.assertRaises(PatchError):
                merge(m, [layer(0, "lang/en.cfg", f'TEXT "title" "{value}"')], "en")
        item = Input("bad", "lang/en.cfg", b"\xff", 0)
        with self.assertRaises(PatchError):
            merge(m, [item], "en")

    def test_unchanged_nonascii_narrow_base_is_rejected(self):
        m = fixture()
        m.classes["UNIT"][1]["info_txt"] = "ёж.inf"
        patch = layer(0, "cfg/patch.cfg", '@patch UNIT "one" ENERGY 12 END')
        with self.assertRaises(PatchError):
            merge(m, [patch], "en")
        self.assertEqual(m.classes["UNIT"][1]["info_txt"], "ёж.inf")

    def test_nonascii_bare_tokens_rejected(self):
        m = fixture()
        for body in ('ENERGY １２', 'ÉNERGY 12'):
            with self.subTest(body=body), self.assertRaises(PatchError):
                merge(m, [layer(0, "cfg/patch.cfg", f'@patch UNIT "one" {body} END')], "en")
        # Unicode after a comment marker is not a dispatch token.
        out = merge(m, [layer(0, "cfg/patch.cfg", '@patch UNIT "one" ENERGY 12 END # комментарий')], "en")
        self.assertEqual(out.classes["UNIT"][0]["energy"], 12)

    def test_repeated_replace_and_slot_clear(self):
        m = fixture()
        weapon = G.new_record("WEAPON")
        weapon["__name__"] = "gun"
        m.classes["WEAPON"].append(weapon)
        p = layer(0, "cfg/patch.cfg", '@replace WEAPON "gun"\nPROBABILYTY 0 4\n@clear PROBABILYTY\nEND\n@patch WEAPON "gun"\nPOWER 14\nEND')
        rec = merge(m, [p], "en").classes["WEAPON"][0]
        self.assertEqual(rec["probability"], {0: 100})
        self.assertEqual(rec["power"], 14)

    def test_area_column_and_bounds(self):
        m = fixture()
        anim = G.new_record("ANIM")
        anim["__name__"] = "build"
        m.classes["ANIM"].append(anim)
        b = G.new_record("BUILDING")
        b["__name__"] = "house"
        b["anims"] = {0: "build"}
        b["build_time"] = 1.0
        b["sprite_quantity"] = 1
        b["area"][3][2] = 1
        b["area"][2][3] = 1
        m.classes["BUILDING"].append(b)
        p = layer(0, "cfg/patch.cfg", '@patch BUILDING "house"\nAREA 2 9\nEND')
        area = merge(m, [p], "en").classes["BUILDING"][0]["area"]
        self.assertEqual((area[0][2], area[3][2], area[2][3]), (9, 0, 1))
        for statement in ('AREA 10 1', 'CAPACITY 10 "ore" 1', 'ENERGY 2147483648'):
            with self.subTest(statement=statement), self.assertRaises(PatchError):
                merge(m, [layer(0, "cfg/patch.cfg", f'@patch BUILDING "house"\n{statement}\nEND')], "en")

    def test_inline_blocks_and_bank_identity(self):
        m = fixture()
        m.def_banks = [("sprites", 0)]
        m.banks = [{"name": "sprites", "sprites": [("s", 3)]}]
        item = layer(0, "cfg/patch.cfg", '@patch BANK "sprites" SPRITE "s" 3 END @patch UNIT "one" ENERGY 12 END')
        result = merge(m, [item], "en")
        self.assertEqual(result.classes["UNIT"][0]["energy"], 12)
        item = layer(0, "cfg/patch.cfg", '@replace BANK "sprites" SPRITE "s" 4 END')
        with self.assertRaises(PatchError):
            merge(m, [item], "en")

    def test_checked_text_index(self):
        m = fixture()
        m.texts.append(("title", "duplicate"))
        with self.assertRaises(PatchError):
            merge(m, [layer(0, "lang/en.cfg", 'TEXT "title" "new"')], "en")
        out = merge(m, [layer(0, "lang/en.cfg", 'TEXT "title" "new" INDEX 7')], "en")
        self.assertEqual(out.texts[6], ("title", "new"))

    def test_shadowed_translation_is_validated(self):
        m = fixture()
        bad = layer(0, "lang/en.cfg", f'TEXT "title" "{"x" * 120}"')
        later = layer(1, "lang/en.cfg", 'TEXT "title" "short"')
        with self.assertRaises(PatchError):
            merge(m, [bad, later], "en")

    def test_unknown_locale_only_allows_gameplay(self):
        m = fixture()
        item = layer(0, "cfg/patch.cfg", '@patch UNIT "one" ENERGY 12 END')
        self.assertEqual(merge(m, [item], "").texts, m.texts)
        with self.assertRaises(PatchError):
            merge(m, [layer(0, "lang/en.cfg", 'TEXT "title" "new"')], "")

    def test_nonascii_narrow_values_rejected(self):
        m = fixture()
        for statement in ('INFO_TXT "ёж.INF"', 'WEAPON 0 "ёж"'):
            with self.subTest(statement=statement), self.assertRaises(PatchError):
                merge(m, [layer(0, "cfg/patch.cfg", f'@patch UNIT "one" {statement} END')], "en")

    def test_modified_integer_bounds(self):
        m = fixture()
        m.globals["UNIT_ENERGY_MUL"] = G.f32(2.0)
        with self.assertRaises(PatchError):
            merge(m, [layer(0, "cfg/patch.cfg", '@patch UNIT "one" ENERGY 2147483647 END')], "en")

    def test_resource_id_and_discriminator(self):
        m = fixture()
        m.defines += [("badore", 6), ("u_type", 1)]
        with self.assertRaises(PatchError):
            merge(m, [layer(0, "cfg/patch.cfg", '@patch UNIT "one" RESOURCE 1 "badore" END')], "en")
        upgrade = G.new_record("UPGRADE")
        upgrade["__name__"] = "up"
        m.classes["UPGRADE"].append(upgrade)
        ok = layer(0, "cfg/patch.cfg", '@patch UPGRADE "up" TYPE "u_type" OBJECT 0 "one" END')
        self.assertEqual(merge(m, [ok], "en").classes["UPGRADE"][0]["objects"][0], "one")
        bad = layer(0, "cfg/patch.cfg", '@patch UPGRADE "up" OBJECT 0 "one" END')
        with self.assertRaises(PatchError):
            merge(m, [bad], "en")

    def test_nonselected_locale_combines_english_fallback(self):
        m = fixture()
        m.defines += [("producer", 20), ("housing", 21), ("housing_type", 7), ("reason", 15)]
        m.texts += [("producer", "p" * 90), ("housing", "h"), ("reason", "r")]
        a = G.new_record("ANIM")
        a["__name__"] = "build"
        m.classes["ANIM"].append(a)
        for name in ("producer", "housing"):
            b = G.new_record("BUILDING")
            b["__name__"] = name
            b["anims"] = {0: "build"}
            b["build_time"] = 1.0
            b["sprite_quantity"] = 1
            if name == "housing":
                b["type"] = "housing_type"
            m.classes["BUILDING"].append(b)
        en = layer(0, "lang/en.cfg", f'TEXT "reason" "{"r" * 90}"')
        other = layer(0, "lang/de.cfg", f'TEXT "housing" "{"h" * 90}"')
        with self.assertRaises(PatchError):
            merge(m, [en, other], "en")

    def test_resource_label_format_and_length(self):
        m = fixture()
        self.assertEqual(merge(m, [layer(0, "lang/en.cfg", 'TEXT "label73" "A%%B"')], "en").texts[2][1], "A%%B")
        for value in ("x" * 20, "bad %s", "bad %"):
            with self.subTest(value=value), self.assertRaises(PatchError):
                merge(m, [layer(0, "lang/en.cfg", f'TEXT "label73" "{value}"')], "en")

    def test_alias_translation_covers_capacity_names(self):
        from .aliases import _translate
        m = fixture()
        building = G.new_record("BUILDING")
        building["__name__"] = "house"
        building["capacity"] = {0: ("ore", 3.0)}
        building["extract_fuel"] = {1: ("ore", 2.0)}
        m.classes["BUILDING"].append(building)
        translated = _translate(m, {"ore": "alloy"})
        self.assertEqual(translated.classes["BUILDING"][0]["capacity"][0][0], "alloy")
        self.assertEqual(translated.classes["BUILDING"][0]["extract_fuel"][1][0], "alloy")
        self.assertEqual(m.classes["BUILDING"][0]["capacity"][0][0], "ore")

    def test_planet_map_name_boundary(self):
        m = fixture()
        planet = G.new_record("PLANET")
        planet["__name__"] = "planet"
        m.classes["PLANET"].append(planet)
        good = "x" * 31
        out = merge(m, [layer(0, "cfg/patch.cfg", f'@patch PLANET "planet" MAP "{good}" END')], "en")
        self.assertEqual(out.classes["PLANET"][0]["map"], good)
        with self.assertRaises(PatchError):
            merge(m, [layer(0, "cfg/patch.cfg", f'@patch PLANET "planet" MAP "{good}x" END')], "en")

    def test_nonconstructible_base_static_animation_is_valid(self):
        m = fixture()
        m.defines.append(("base_type", 34))
        anim = G.new_record("ANIM")
        anim["__name__"] = "base_animation"
        m.classes["ANIM"].append(anim)
        base = G.new_record("BUILDING")
        base["__name__"] = "base"
        base["type"] = "base_type"
        base["sprite_quantity"] = 1
        base["anims"] = {0: "base_animation", 1: "base_animation"}
        base["build_time"] = 0.0
        m.classes["BUILDING"].append(base)
        out = merge(m, [layer(0, "cfg/patch.cfg", '@patch UNIT "one" ENERGY 12 END')], "en")
        self.assertEqual(out.classes["BUILDING"][0]["build_time"], 0.0)

    def test_generic_online_zero_time_rejected_after_retime(self):
        m = fixture()
        m.defines.append(("factory_type", 1))
        for name in ("construction", "online"):
            anim = G.new_record("ANIM")
            anim["__name__"] = name
            m.classes["ANIM"].append(anim)
        factory = G.new_record("BUILDING")
        factory["__name__"] = "factory"
        factory["type"] = "factory_type"
        factory["sprite_quantity"] = 1
        factory["build_time"] = 1.0
        factory["anims"] = {0: "construction", 1: "online"}
        m.classes["BUILDING"].append(factory)
        with self.assertRaises(PatchError):
            merge(m, [layer(0, "cfg/patch.cfg", '@patch ANIM "online" TIME 0 END')], "en")

    def test_guarded_online_allows_zero_but_rejects_negative(self):
        m = fixture()
        m.defines.append(("mother_type", 6))
        for name in ("construction", "online"):
            anim = G.new_record("ANIM")
            anim["__name__"] = name
            m.classes["ANIM"].append(anim)
        mother = G.new_record("BUILDING")
        mother["__name__"] = "mother"
        mother["type"] = "mother_type"
        mother["sprite_quantity"] = 1
        mother["build_time"] = 1.0
        mother["anims"] = {0: "construction", 4: "online"}
        m.classes["BUILDING"].append(mother)
        zero = layer(0, "cfg/patch.cfg", '@patch ANIM "online" TIME 0 END')
        self.assertEqual(merge(m, [zero], "en").classes["ANIM"][1]["time"], 0.0)
        negative = layer(0, "cfg/patch.cfg", '@patch ANIM "online" TIME -1 END')
        with self.assertRaises(PatchError):
            merge(m, [negative], "en")
        zero_mother = layer(0, "cfg/patch.cfg", '@patch BUILDING "mother" BUILD_TIME 0 END')
        with self.assertRaises(PatchError):
            merge(m, [zero_mother], "en")

    def test_airfield_sentinel_slot_is_not_ticked(self):
        m = fixture()
        m.defines.append(("airfield_type", 29))
        for name in ("construction", "online", "sentinel"):
            anim = G.new_record("ANIM")
            anim["__name__"] = name
            m.classes["ANIM"].append(anim)
        b = G.new_record("BUILDING")
        b["__name__"] = "airfield"
        b["type"] = "airfield_type"
        b["sprite_quantity"] = 3
        b["build_time"] = 1.0
        b["anims"] = {0: "construction", 1: "online", 2: "online", 3: "sentinel"}
        m.classes["BUILDING"].append(b)
        ignored = layer(0, "cfg/patch.cfg", '@patch ANIM "sentinel" TIME 0 END')
        self.assertEqual(merge(m, [ignored], "en").classes["ANIM"][2]["time"], 0.0)
        active = layer(0, "cfg/patch.cfg", '@patch ANIM "online" TIME 0 END')
        with self.assertRaises(PatchError):
            merge(m, [active], "en")


if __name__ == "__main__":
    unittest.main()
