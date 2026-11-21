"""Wiki selftest: synthetic data only, no retail files.  ``python -m src.formats.wiki --selftest``.

Builds the model from test_model's synthetic INIT/INITLANG (EN+RU)/INFO.TXT, runs its formula
tests, builds a site with 4x4 synthetic icons (no sprites/planets/stills: art extraction needs the
retail install, so it is skipped), runs every site check, and requires the rendered site and the
tech-tree layout to be byte-identical across two runs.
"""

from __future__ import annotations

import hashlib
import shutil
import sys
import tempfile
import time
from pathlib import Path


def _digest(root):
    h = hashlib.sha256()
    for p in sorted(Path(root).rglob("*")):
        if p.is_file():
            h.update(p.relative_to(root).as_posix().encode())
            h.update(p.read_bytes())
    return h.hexdigest()


def _synthetic_icons(wiki, art_dir):
    from PIL import Image

    icons = {}
    d = Path(art_dir) / "icons"
    d.mkdir(parents=True, exist_ok=True)
    for kind in ("unit", "building", "project", "weapon", "planet", "system"):
        for e in wiki.by_kind[kind]:
            if e.icon and e.icon not in icons:
                Image.new("RGBA", (4, 4), (200, 40, 40, 255)).save(d / f"{e.icon}.png")
                icons[e.icon] = f"icons/{e.icon}.png"
    return icons


def _synthetic_maps(wiki, art_dir):
    """Icons plus a star-map background and a planet frame for the first system."""
    from PIL import Image

    maps = {
        "icons": _synthetic_icons(wiki, art_dir),
        "buildings": {},
        "units": {},
        "planets": {},
        "stills": {},
        "starmaps": {},
        "planetmaps": {},
        "planetthumbs": {},
        "planetmapinfo": {},
    }
    for sub in ("planets", "starmaps"):
        (Path(art_dir) / sub).mkdir(parents=True, exist_ok=True)
    Image.new("RGB", (16, 10), (20, 10, 40)).save(Path(art_dir) / "starmaps/n_uklad1.jpg")
    maps["starmaps"]["N_uklad1"] = "starmaps/n_uklad1.jpg"
    for pl in wiki.by_kind["planet"]:
        rel = f"planets/{pl.cfg_name.lower()}.png"
        Image.new("RGBA", (6, 6), (90, 120, 200, 255)).save(Path(art_dir) / rel)
        maps["planets"][pl.cfg_name] = rel
    # a planet map preview (the real one needs retail .MP data) for the first planet only
    (Path(art_dir) / "planetmaps").mkdir(parents=True, exist_ok=True)
    first = wiki.by_kind["planet"][0].cfg_name
    Image.new("RGB", (32, 32), (30, 90, 40)).save(Path(art_dir) / "planetmaps/p1.jpg")
    Image.new("RGB", (16, 16), (30, 90, 40)).save(Path(art_dir) / "planetmaps/p1_t.jpg")
    maps["planetmaps"][first] = "planetmaps/p1.jpg"
    maps["planetthumbs"][first] = "planetmaps/p1_t.jpg"
    maps["planetmapinfo"][first] = {
        "w": 64,
        "h": 64,
        "map": "p1.mp",
        "land": [[8, 8], [56, 56]],
        "ore": [[16, 32]],
    }
    return maps


def _check_still_rules(wiki):
    """A still shared by more than STILL_SHARED_MAX entities, and any project still, is not shown."""
    from . import site

    same = ["N_human_Soldier3", "N_human_Soldier1", "N_human_Tank", "N_alien_Jet"]
    touched = [(wiki.by_cfg("unit", n), r"res\SAME.AVI") for n in same]
    touched.append((wiki.by_cfg("building", "N_human_Port"), r"res\ONE.AVI"))
    touched.append((wiki.by_cfg("project", "N_human_pr_Tank"), r"res\ONE.AVI"))
    old = [(e, e.art.get("info_flc")) for e, _ in touched]
    try:
        for e, v in touched:
            e.art["info_flc"] = v
        m = site.build_mapping(
            wiki, {"stills": {"res/same.avi": "stills/s.png", "res/one.avi": "stills/o.png"}}
        )
        assert all(not m.ref(e).still for e in (wiki.by_cfg("unit", n) for n in same)), "shared"
        assert m.ref(wiki.by_cfg("building", "N_human_Port")).still == "stills/o.png"
        assert not m.ref(wiki.by_cfg("project", "N_human_pr_Tank")).still, "project still shown"
        assert not m.errors, m.errors  # skipped stills still count as claimed (no orphans)
    finally:
        for e, v in old:
            if v is None:
                e.art.pop("info_flc", None)
            else:
                e.art["info_flc"] = v


def _check_side_markers(wiki, maps):
    """Two links with one display name and different factions get a side marker, others do not."""
    from . import site

    st = site.Site(wiki, maps, ("en", "ru"))
    tank, jet = wiki.by_cfg("unit", "N_human_Tank"), wiki.by_cfg("unit", "N_alien_Jet")
    reactor = wiki.by_cfg("building", "N_alien_Reactor")
    links = [wiki.link(tank), wiki.link(jet), wiki.link(reactor)]
    en = site.Ctx(st, "en", "x/x")
    assert en.ambiguous(links) == set(), "no marker without a name collision"
    old = dict(jet.names)
    try:
        jet.names["en"] = "Tank"
        amb = en.ambiguous(links)
        assert amb == {("unit", tank.slug), ("unit", jet.slug)}, amb
        assert (
            en.side(wiki.link(jet), amb) == "(Alien)" and en.side(wiki.link(tank), amb) == "(Human)"
        )
        assert en.side(wiki.link(reactor), amb) == ""
        assert not site.Ctx(st, "ru", "x/x").ambiguous(links), "collision is per language"
        assert not en.ambiguous([wiki.link(tank), wiki.link(tank)]), "same entity twice"
    finally:
        jet.names.clear()
        jet.names.update(old)


def _check_planet_maps(out):
    """Planet page shows the map preview with markers and size; the index shows the thumbnail."""
    page = (out / "en/planets/planeta_1.html").read_text(encoding="utf-8")
    assert 'class="planetmap"' in page and "64 x 64 tiles" in page, "planet map section"
    assert page.count('class="mk land"') == 3 and page.count('class="mk ore"') == 2  # + legend
    assert "planetmaps/p1.jpg" in page
    ru = (out / "ru/planets/planeta_1.html").read_text(encoding="utf-8")
    assert "64 x 64 клеток" in ru
    assert "planetmaps/p1_t.jpg" in (out / "en/planets/index.html").read_text(encoding="utf-8")
    assert 'class="planetmap"' not in (out / "en/planets/planeta_2.html").read_text(
        encoding="utf-8"
    )
    assert (out / "assets/planetmaps/p1.jpg").is_file()


def _check_pages(out, wiki):
    """Combined building pages, upgrade stubs, system star map, tech trees."""
    page = (out / "en/buildings/human_factory.html").read_text(encoding="utf-8")
    for anchor in ("base", "upgrade-1", "upgrade-2"):
        assert f'id="{anchor}"' in page, f"factory page lacks {anchor}"
    assert "Mega Factory" in page and "Heavy Factory" in page
    stub = (out / "en/buildings/human_factory2.html").read_text(encoding="utf-8")
    assert 'http-equiv="refresh"' in stub and "human_factory.html#upgrade-1" in stub
    idx = (out / "en/buildings/index.html").read_text(encoding="utf-8")
    assert 'href="human_factory.html"' in idx and "human_factory2" not in idx
    # links to an upgrade level land on its anchor of the combined page
    tank = (out / "en/units/human_tank.html").read_text(encoding="utf-8")
    assert "human_factory2.html" not in tank and "human_factory.html" in tank
    sysp = (out / "en/systems/uklad1.html").read_text(encoding="utf-8")
    assert 'class="starmap"' in sysp and sysp.count('class="sm-planet"') == 2
    assert "left:3.125%" in sysp  # 32 / 1024
    assert 'class="starmap"' not in (out / "en/systems/uklad2.html").read_text(encoding="utf-8")
    for f in ("alien", "human"):
        t = (out / f"en/techtree/{f}.html").read_text(encoding="utf-8")
        assert "V_human_baza" not in t and "V_human_chatka1" not in t, "unbuildable in tree"
        assert "tt-edge gate derived" in t and "game rule" in t, "derived edges / legend"
        assert "merging single soldiers" in t, "squad note"
        assert "V_human_s3" not in t, "multi-soldier squad in the tree"
    assert "V_human_s1" in (out / "en/techtree/human.html").read_text(encoding="utf-8")
    assert (out / "en/units/human_soldier3.html").is_file()  # the squad's page stays
    assert "Base" in (out / "en/buildings/human_baza.html").read_text(
        encoding="utf-8"
    )  # page stays


def run():
    from . import site, techtree, test_model

    steps = []

    def step(name, fn):
        t = time.time()
        fn()
        steps.append((name, time.time() - t))

    step("model tests", test_model.main)
    wiki = test_model.W
    langs = ("en", "ru")
    tmp = Path(tempfile.mkdtemp(prefix="wiki_selftest_"))
    try:
        art_dir = tmp / "art"
        maps = _synthetic_maps(wiki, art_dir)
        assert maps["icons"], "synthetic model has no icons"
        digests = []
        for i in (1, 2):
            out = tmp / f"site{i}"

            def build(out=out):
                rep = site.build_site(wiki, maps, out, langs=langs, art_dir=art_dir, strict=True)
                assert rep.search_entries > 0, "empty search index"
                problems = site.run_checks(wiki, rep.mapping, out, langs)
                bad = {k: v[:3] for k, v in problems.items() if v}
                assert not bad, f"site checks failed: {bad}"

            step(f"site build+checks #{i}", build)
            digests.append(_digest(out))
        step("pages", lambda: _check_pages(tmp / "site1", wiki))
        step("planet maps", lambda: _check_planet_maps(tmp / "site1"))
        step("side markers", lambda: _check_side_markers(wiki, maps))
        step("still rules", lambda: _check_still_rules(wiki))
        assert digests[0] == digests[1], "site output differs between two builds"

        def tree():
            runs = []
            for _ in range(2):
                g = techtree.build_graph(wiki.cfg)
                lays = {
                    f: techtree.layout(techtree.faction_graph(g, f)) for f in ("alien", "human")
                }
                runs.append(repr(lays))
            assert runs[0] == runs[1], "tech-tree layout not deterministic"
            techtree.selftest()

        step("tech-tree layout", tree)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return steps


def main():
    t0 = time.time()
    try:
        steps = run()
    except AssertionError as e:
        print(f"selftest FAILED: {e}", file=sys.stderr)
        return 1
    for name, s in steps:
        print(f"  {name}: {s:.2f}s")
    print(
        f"ffmpeg present: {shutil.which('ffmpeg') is not None} (art extraction skipped: retail only)"
    )
    print(f"wiki selftest OK in {time.time() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
